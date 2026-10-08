#include "mp/net_server.h"

#include <cerrno>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mp::net
{

#if defined(_WIN32)

// TODO: Winsock version. Until then the server is not available on Windows.
bool start( int, std::string &error )
{
    error = "the network server is not supported on Windows yet";
    return false;
}
void stop() {}
bool running()
{
    return false;
}
bool has_client()
{
    return false;
}
void poll( const handlers & ) {}
void send_line( const std::string & ) {}

#else

namespace
{

int listen_fd = -1;
int client_fd = -1;
std::string in_buffer;
std::string out_buffer;
// Longest line we accept; protects against a client that never sends '\n'.
constexpr size_t max_line = 64 * 1024;

#if defined(MSG_NOSIGNAL)
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

// The call would block or was interrupted; try again on the next poll.
bool is_transient_error( const int err )
{
#if EAGAIN != EWOULDBLOCK
    if( err == EWOULDBLOCK ) {
        return true;
    }
#endif
    return err == EAGAIN || err == EINTR;
}

bool set_non_blocking( const int fd )
{
    const int flags = fcntl( fd, F_GETFL, 0 );
    return flags >= 0 && fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
}

void close_client()
{
    if( client_fd >= 0 ) {
        close( client_fd );
        client_fd = -1;
    }
    in_buffer.clear();
    out_buffer.clear();
}

} // namespace

bool start( const int port, std::string &error )
{
    if( listen_fd >= 0 ) {
        return true;
    }
    const int fd = socket( AF_INET, SOCK_STREAM, 0 );
    if( fd < 0 ) {
        error = std::strerror( errno );
        return false;
    }
    const int yes = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof( yes ) );
    sockaddr_in addr;
    std::memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( static_cast<uint16_t>( port ) );
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if( bind( fd, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) != 0 ||
        listen( fd, 1 ) != 0 || !set_non_blocking( fd ) ) {
        error = std::strerror( errno );
        close( fd );
        return false;
    }
    listen_fd = fd;
    return true;
}

void stop()
{
    close_client();
    if( listen_fd >= 0 ) {
        close( listen_fd );
        listen_fd = -1;
    }
}

bool running()
{
    return listen_fd >= 0;
}

bool has_client()
{
    return client_fd >= 0;
}

void send_line( const std::string &line )
{
    if( client_fd < 0 ) {
        return;
    }
    out_buffer += line;
    out_buffer += '\n';
}

static bool flush_output()
{
    while( !out_buffer.empty() ) {
        const ssize_t sent = send( client_fd, out_buffer.data(), out_buffer.size(), send_flags );
        if( sent < 0 ) {
            return is_transient_error( errno );
        }
        out_buffer.erase( 0, static_cast<size_t>( sent ) );
    }
    return true;
}

void poll( const handlers &h )
{
    if( listen_fd < 0 ) {
        return;
    }
    const int fd = accept( listen_fd, nullptr, nullptr );
    if( fd >= 0 ) {
        if( client_fd >= 0 || !set_non_blocking( fd ) ) {
            static const std::string busy =
                R"({"type":"error","message":"another player is already connected"})" "\n";
            send( fd, busy.data(), busy.size(), send_flags );
            close( fd );
        } else {
            client_fd = fd;
            if( h.on_connect ) {
                h.on_connect();
            }
        }
    }
    if( client_fd < 0 ) {
        return;
    }

    bool lost = !flush_output();
    char buf[4096];
    while( !lost ) {
        const ssize_t got = recv( client_fd, buf, sizeof( buf ), 0 );
        if( got > 0 ) {
            in_buffer.append( buf, static_cast<size_t>( got ) );
        } else if( got == 0 ) {
            lost = true;
        } else {
            lost = !is_transient_error( errno );
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

#endif

} // namespace mp::net
