#pragma once
#ifndef CATA_SRC_MP_RC_NPC_H
#define CATA_SRC_MP_RC_NPC_H

#include "coordinates.h"

class JsonObject;
class JsonOut;
class npc;

// Remote-controlled NPC (RC-NPC): an NPC driven by a queue of commands from
// the second player instead of the AI. See docs/mp/architecture-notes.md.
namespace mp
{

bool is_remote( const npc &guy );
void set_remote( npc &guy, bool remote );

enum class command_type : int {
    move,       // step to an adjacent tile, attacking a hostile creature there
    attack,     // melee attack whatever creature is on an adjacent tile
    wait,       // spend the turn doing nothing
    pickup_all, // pick up every item on the tile the NPC stands on
};

struct command {
    command_type type = command_type::wait;
    // Direction for move and attack.
    point_rel_ms dir;
};

void push_command( const npc &guy, const command &cmd );
bool has_commands( const npc &guy );

// Hook for npc::move(): runs queued commands instead of the AI.
// Returns false if guy is not remote-controlled.
bool remote_move( npc &guy );
// True if guy is remote-controlled and has nothing to do until a new
// command arrives. monmove() stops calling npc::move() for such NPCs and
// leaves them their remaining moves.
bool waits_for_commands( const npc &guy );

// Hooks for npc::store() / npc::load() in savegame_json.cpp.
void store_npc( const npc &guy, JsonOut &json );
void load_npc( npc &guy, const JsonObject &data );

// Entry of the debug menu: toggle the flag, queue commands by hand.
void debug_menu();

} // namespace mp

#endif // CATA_SRC_MP_RC_NPC_H
