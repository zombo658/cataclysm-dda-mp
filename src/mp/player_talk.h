#pragma once
#ifndef CATA_SRC_MP_PLAYER_TALK_H
#define CATA_SRC_MP_PLAYER_TALK_H

#include <string>

class JsonObject;
class npc;

// What the two players do with each other: messages (only between them,
// not heard in the game's world), looking at the other's wounds, trading
// places and things.
namespace mp::player_talk
{

// ---- Host ----

// The second player stepped into the host or talks to them: a menu of what
// to do (asked of the second player).
void host_menu( npc &guy );
// The "say" command: a message from the second player.
void say_from_client( const npc &guy, const std::string &text );
// Hook in avatar::talk_to(): the host talks to the second player's
// character; a message instead of an NPC's dialogue. True if it was that.
bool host_talks_to( const npc &who );

// ---- Client ----

// A "chat" message from the host.
void show( const JsonObject &message );
// An "inspect" question: the host's wounds or status, on the copy.
void inspect( const JsonObject &question );

} // namespace mp::player_talk

#endif // CATA_SRC_MP_PLAYER_TALK_H
