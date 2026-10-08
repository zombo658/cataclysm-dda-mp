#pragma once
#ifndef CATA_SRC_MP_PROTOCOL_H
#define CATA_SRC_MP_PROTOCOL_H

#include <string>

class npc;

// JSON messages between the game and the second player's client.
// The format is described in docs/mp/protocol.md.
namespace mp::protocol
{

constexpr int version = 3;

// Accepts a connection, reads commands into the remote NPC's queue and sends
// answers. Call often; it never blocks.
void poll();

void send_welcome();
// What guy sees around itself.
void send_view( const npc &guy );
// The world waits for guy's command. Sends the view first.
void send_your_turn( const npc &guy );
// The view, the status and new log lines, after the client acted.
void send_state( const npc &guy );
// The same, at most a few times a second; called every turn so that the
// client sees the world move while the host plays.
void send_state_if_due();
// A queued command could not be carried out.
void send_rejected( const std::string &reason );

} // namespace mp::protocol

#endif // CATA_SRC_MP_PROTOCOL_H
