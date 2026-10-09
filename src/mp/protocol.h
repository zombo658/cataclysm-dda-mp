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
// Shared time: commands wait for the second player's character to have time.
bool has_deferred();

void send_welcome();

// Item references (item_location) that come over the network point at the
// other side's copy of the items, whose unique ids differ from this side's
// (each load of the JSON gives the items new ids): they are read by index,
// as the game read them before items had ids. Hook in
// item_location::deserialize(). True on the client, and on the host while
// it reads a message.
bool ignore_item_uids();
struct reading_network {
    reading_network();
    ~reading_network();
    reading_network( const reading_network & ) = delete;
    reading_network &operator=( const reading_network & ) = delete;
};
// What guy sees around itself.
void send_view( const npc &guy );
// The world waits for guy's command. Sends the view first.
void send_your_turn( const npc &guy );
// The view, the status and new log lines, after the client acted.
void send_state( const npc &guy );
// The same, at most a few times a second; called every turn so that the
// client sees the world move while the host plays.
void send_state_if_due();
// The state send_state_if_due() held back, once the limit allows it.
void send_pending_state();
// Something the client sees changed (the second player's activity went on).
void mark_changed();
// A queued command could not be carried out.
void send_rejected( const std::string &reason );

} // namespace mp::protocol

#endif // CATA_SRC_MP_PROTOCOL_H
