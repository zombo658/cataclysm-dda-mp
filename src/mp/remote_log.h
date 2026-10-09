#pragma once
#ifndef CATA_SRC_MP_REMOTE_LOG_H
#define CATA_SRC_MP_REMOTE_LOG_H

#include <string>

#include "enums.h"

class Character;
class JsonObject;

// The second player's own message log. The game writes messages "to the
// player" (add_msg_if_player(), the player part of add_msg_player_or_npc())
// only for the avatar; for the second player's NPC they go to the client
// instead, in the second person. The host's own such messages ("You feel
// hungry") are not passed on to the client.
namespace mp::remote_log
{

// ---- Host ----

// Hooks in npc::add_msg_if_player() and the npc versions of
// add_msg_player_or_npc()/add_msg_player_or_say(): true if who is the
// second player's character (then the message went to the client).
bool to_second_player( const Character &who, const std::string &text,
                       game_message_type type = m_neutral );
// The third person version that the host's log gets for the same event:
// already told to the second player, not sent again.
void told_already( const std::string &text );
// Hooks in Character::add_msg_if_player() and co.: a message for the host
// only.
void host_only( const Character &who, const std::string &text );
// Whether a line of the host's log is for the second player too.
bool is_shared( const std::string &line );

// ---- Client ----

void read( const JsonObject &message );

} // namespace mp::remote_log

#endif // CATA_SRC_MP_REMOTE_LOG_H
