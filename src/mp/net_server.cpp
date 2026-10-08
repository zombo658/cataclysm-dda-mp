#include "mp/net_server.h"

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
#include <netinet/in.h>
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

std::string error_text( const int err )
{
    return "socket error " + std::to_string( err );
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
// Don't kill the game with SIGPIPE when the client goes away.
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

socket_t listen_socket = no_socket;
socket_t client_socket = no_socket;
std::string in_buffer;
std::string out_buffer;
// Longest line we accept; protects against a client that never sends '\n'.
constexpr size_t max_line = 64 * 1024;

void close_client()
{
    if( client_socket != no_socket ) {
        close_socket( client_socket );
        client_socket = no_socket;
    }
    in_buffer.clear();
    out_buffer.clear();
}

bool flush_output()
{
    while( !out_buffer.empty() ) {
        const int sent = send_bytes( client_socket, out_buffer.data(), out_buffer.size() );
        if( sent < 0 ) {
            return is_transient_error( last_error() );
        }
        out_buffer.erase( 0, static_cast<size_t>( sent ) );
    }
    return true;
}

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
    close_client();
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
    return client_socket != no_socket;
}

void send_line( const std::string &line )
{
    if( client_socket == no_socket ) {
        return;
    }
    out_buffer += line;
    out_buffer += '\n';
}

void poll( const handlers &h )
{
    if( listen_socket == no_socket ) {
        return;
    }
    const socket_t s = accept( listen_socket, nullptr, nullptr );
    if( s != no_socket ) {
        if( client_socket != no_socket || !set_non_blocking( s ) ) {
            static const std::string busy =
                R"({"type":"error","message":"another player is already connected"})" "\n";
            send_bytes( s, busy.data(), busy.size() );
            close_socket( s );
        } else {
            client_socket = s;
            if( h.on_connect ) {
                h.on_connect();
            }
        }
    }
    if( client_socket == no_socket ) {
        return;
    }

    bool lost = !flush_output();
    char buf[4096];
    while( !lost ) {
        const int got = recv_bytes( client_socket, buf, sizeof( buf ) );
        if( got > 0 ) {
            in_buffer.append( buf, static_cast<size_t>( got ) );
        } else if( got == 0 ) {
            lost = true;
        } else {
            lost = !is_transient_error( last_error() );
            break;
        }
    }

    size_t eol;
    while( ( eol = in_buffer.find( '\n' ) ) != std::string::npos ) {
        std::string line = in_buffer.substr( 0, eol );
        in_buffer.erase( 0, eol + 1 );
        if( !line.empty() && line.back() == '\r' ) {
            line.pop_back();
        }
        if( !line.empty() && h.on_line ) {
            h.on_line( line );
        }
    }
    if( in_buffer.size() > max_line ) {
        lost = true;
    }
    // Answers to the lines just handled.
    if( !lost ) {
        lost = !flush_output();
    }
    if( lost ) {
        close_client();
        if( h.on_disconnect ) {
            h.on_disconnect();
        }
    }
}

} // namespace mp::net
