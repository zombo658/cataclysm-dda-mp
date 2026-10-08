#pragma once
#ifndef CATA_SRC_MP_NET_SERVER_H
#define CATA_SRC_MP_NET_SERVER_H

#include <functional>
#include <string>

// Non-blocking TCP server for one client. Messages are lines of text
// (NDJSON: one JSON object per line). Nothing here knows about the game.
namespace mp::net
{

constexpr int default_port = 7777;

// Returns false and fills error if the port can't be opened.
bool start( int port, std::string &error );
void stop();
bool running();
bool has_client();

// Accepts a pending connection, sends queued output and reads input without
// blocking. Calls on_line for every complete line received, on_connect when
// a client connects and on_disconnect when it goes away.
struct handlers {
    std::function<void( const std::string & )> on_line;
    std::function<void()> on_connect;
    std::function<void()> on_disconnect;
};
void poll( const handlers &h );

// Queues one line for the client (a newline is appended). Dropped if no
// client is connected.
void send_line( const std::string &line );

} // namespace mp::net

#endif // CATA_SRC_MP_NET_SERVER_H
