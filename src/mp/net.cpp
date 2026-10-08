#include "mp/net.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#   define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#   define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mp::net
{

namespace
{

// The few places where Winsock and POSIX sockets differ.
#if defined(_WIN32)
using socket_t = SOCKET;
const socket_t no_socket = INVALID_SOCKET;
constexpr int send_flags = 0;

bool init_sockets( std::string &error )
{
    static bool done = false;
    WSADATA data;
    if( !done && WSAStartup( MAKEWORD( 2, 2 ), &data ) != 0 ) {
        error = "WSAStartup failed";
        return false;
    }
    done = true;
    return true;
}

void close_socket( const socket_t s )
{
    closesocket( s );
}

bool set_non_blocking( const socket_t s )
{
    u_long on = 1;
    return ioctlsocket( s, FIONBIO, &on ) == 0;
}

int last_error()
{
    return WSAGetLastError();
}

// The call would block or was interrupted; try again on the next poll.
bool is_transient_error( const int err )
{
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

// A non-blocking connect() has started and goes on in the background.
bool is_connect_in_progress( const int err )
{
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

std::string error_text( const int err )
{
    switch( err ) {
        case WSAECONNREFUSED:
            return "connection refused (is the server started?)";
        case WSAETIMEDOUT:
            return "connection timed out";
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
            return "host unreachable";
        case WSAEADDRINUSE:
            return "address already in use";
        default:
            return "socket error " + std::to_string( err );
    }
}

int send_bytes( const socket_t s, const char *data, const size_t size )
{
    return ::send( s, data, static_cast<int>( size ), send_flags );
}

int recv_bytes( const socket_t s, char *data, const size_t size )
{
    return ::recv( s, data, static_cast<int>( size ), 0 );
}
#else
using socket_t = int;
const socket_t no_socket = -1;
#if defined(MSG_NOSIGNAL)
// Don't kill the game with SIGPIPE when the other side goes away.
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

bool init_sockets( std::string & )
{
    return true;
}

void close_socket( const socket_t s )
{
    close( s );
}

bool set_non_blocking( const socket_t s )
{
    const int flags = fcntl( s, F_GETFL, 0 );
    return flags >= 0 && fcntl( s, F_SETFL, flags | O_NONBLOCK ) == 0;
}

int last_error()
{
    return errno;
}

bool is_transient_error( const int err )
{
#if EAGAIN != EWOULDBLOCK
    if( err == EWOULDBLOCK ) {
        return true;
    }
#endif
    return err == EAGAIN || err == EINTR;
}

bool is_connect_in_progress( const int err )
{
    return err == EINPROGRESS;
}

std::string error_text( const int err )
{
    return std::strerror( err );
}

int send_bytes( const socket_t s, const char *data, const size_t size )
{
    return static_cast<int>( ::send( s, data, size, send_flags ) );
}

int recv_bytes( const socket_t s, char *data, const size_t size )
{
    return static_cast<int>( ::recv( s, data, size, 0 ) );
}
#endif

// One connected peer with its line buffers.
struct connection {
    socket_t s = no_socket;
    std::string in;
    std::string out;

    bool is_open() const {
        return s != no_socket;
    }

    void close() {
        if( s != no_socket ) {
            close_socket( s );
            s = no_socket;
        }
        in.clear();
        out.clear();
    }

    void send_line( const std::string &line ) {
        if( s == no_socket ) {
            return;
        }
        out += line;
        out += '\n';
    }

    bool flush() {
        while( !out.empty() ) {
            const int sent = send_bytes( s, out.data(), out.size() );
            if( sent < 0 ) {
                return is_transient_error( last_error() );
            }
            out.erase( 0, static_cast<size_t>( sent ) );
        }
        return true;
    }

    // Sends and receives what it can; returns false if the peer is gone.
    bool exchange( const handlers &h ) {
        // Longest line we accept; protects against a peer that never sends '\n'.
        constexpr size_t max_line = 1024 * 1024;
        bool lost = !flush();
        char buf[16384];
        while( !lost ) {
            const int got = recv_bytes( s, buf, sizeof( buf ) );
            if( got > 0 ) {
                in.append( buf, static_cast<size_t>( got ) );
            } else if( got == 0 ) {
                lost = true;
            } else {
                lost = !is_transient_error( last_error() );
                break;
            }
        }
        size_t eol;
        while( ( eol = in.find( '\n' ) ) != std::string::npos ) {
            std::string line = in.substr( 0, eol );
            in.erase( 0, eol + 1 );
            if( !line.empty() && line.back() == '\r' ) {
                line.pop_back();
            }
            if( !line.empty() && h.on_line ) {
                h.on_line( line );
            }
        }
        if( in.size() > max_line ) {
            lost = true;
        }
        // Answers to the lines just handled.
        return !lost && flush();
    }
};

socket_t listen_socket = no_socket;
connection server_side;
connection client_side;

} // namespace

bool start( const int port, std::string &error )
{
    if( listen_socket != no_socket ) {
        return true;
    }
    if( !init_sockets( error ) ) {
        return false;
    }
    const socket_t s = socket( AF_INET, SOCK_STREAM, 0 );
    if( s == no_socket ) {
        error = error_text( last_error() );
        return false;
    }
#if !defined(_WIN32)
    // On Windows SO_REUSEADDR lets another program steal the port; the
    // default there already allows a quick restart.
    const int yes = 1;
    setsockopt( s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof( yes ) );
#endif
    sockaddr_in addr;
    std::memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( static_cast<uint16_t>( port ) );
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if( bind( s, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) != 0 ||
        listen( s, 1 ) != 0 || !set_non_blocking( s ) ) {
        error = error_text( last_error() );
        close_socket( s );
        return false;
    }
    listen_socket = s;
    return true;
}

void stop()
{
    server_side.close();
    if( listen_socket != no_socket ) {
        close_socket( listen_socket );
        listen_socket = no_socket;
    }
}

bool running()
{
    return listen_socket != no_socket;
}

bool has_client()
{
    return server_side.is_open();
}

void send_line( const std::string &line )
{
    server_side.send_line( line );
}

void poll( const handlers &h )
{
    if( listen_socket == no_socket ) {
        return;
    }
    const socket_t s = accept( listen_socket, nullptr, nullptr );
    if( s != no_socket ) {
        if( server_side.is_open() || !set_non_blocking( s ) ) {
            static const std::string busy =
                R"({"type":"error","message":"another player is already connected"})" "\n";
            send_bytes( s, busy.data(), busy.size() );
            close_socket( s );
        } else {
            server_side.s = s;
            if( h.on_connect ) {
                h.on_connect();
            }
        }
    }
    if( server_side.is_open() && !server_side.exchange( h ) ) {
        server_side.close();
        if( h.on_disconnect ) {
            h.on_disconnect();
        }
    }
}

bool connect_to( const std::string &host, const int port, const int timeout_ms,
                 std::string &error )
{
    disconnect();
    if( !init_sockets( error ) ) {
        return false;
    }
    addrinfo hints;
    std::memset( &hints, 0, sizeof( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    const std::string port_text = std::to_string( port );
    if( getaddrinfo( host.c_str(), port_text.c_str(), &hints, &found ) != 0 || found == nullptr ) {
        error = "can't find host " + host;
        return false;
    }
    const socket_t s = socket( found->ai_family, found->ai_socktype, found->ai_protocol );
    if( s == no_socket || !set_non_blocking( s ) ) {
        error = error_text( last_error() );
        if( s != no_socket ) {
            close_socket( s );
        }
        freeaddrinfo( found );
        return false;
    }
    const int rc = connect( s, found->ai_addr, static_cast<int>( found->ai_addrlen ) );
    freeaddrinfo( found );
    if( rc != 0 && !is_connect_in_progress( last_error() ) ) {
        error = error_text( last_error() );
        close_socket( s );
        return false;
    }
    if( rc != 0 ) {
        // Wait for the connection to finish, at most timeout_ms.
        fd_set writable;
        FD_ZERO( &writable );
        FD_SET( s, &writable );
        fd_set failed;
        FD_ZERO( &failed );
        FD_SET( s, &failed );
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = ( timeout_ms % 1000 ) * 1000;
        const int ready = select( static_cast<int>( s + 1 ), nullptr, &writable, &failed, &tv );
        int so_error = 0;
        socklen_t len = sizeof( so_error );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        getsockopt( s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>( &so_error ), &len );
        if( ready <= 0 || so_error != 0 ) {
            error = ready <= 0 ? "no answer from " + host : error_text( so_error );
            close_socket( s );
            return false;
        }
    }
    client_side.s = s;
    return true;
}

void disconnect()
{
    client_side.close();
}

bool connected()
{
    return client_side.is_open();
}

void client_poll( const handlers &h )
{
    if( client_side.is_open() && !client_side.exchange( h ) ) {
        client_side.close();
        if( h.on_disconnect ) {
            h.on_disconnect();
        }
    }
}

void client_send_line( const std::string &line )
{
    client_side.send_line( line );
}

} // namespace mp::net
