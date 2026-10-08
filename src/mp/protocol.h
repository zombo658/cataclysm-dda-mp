#pragma once
#ifndef CATA_SRC_MP_PROTOCOL_H
#define CATA_SRC_MP_PROTOCOL_H

#include <string>

class npc;

// JSON messages between the game and the second player's client.
// The format is described in docs/mp/protocol.md.
namespace mp::protocol
{

constexpr int version = 1;

// Accepts a connection, reads commands into the remote NPC's queue and sends
// answers. Call often; it never blocks.
void poll();

void send_welcome();
// The world waits for guy's command.
void send_your_turn( const npc &guy );
// A queued command could not be carried out.
void send_rejected( const std::string &reason );

} // namespace mp::protocol

#endif // CATA_SRC_MP_PROTOCOL_H
