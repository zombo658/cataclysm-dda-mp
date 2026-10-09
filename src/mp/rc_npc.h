#pragma once
#ifndef CATA_SRC_MP_RC_NPC_H
#define CATA_SRC_MP_RC_NPC_H

#include <functional>

#include "action.h"
#include "coordinates.h"

class Character;
class JsonObject;
class JsonOut;
class map;
class npc;
class vehicle;

// Remote-controlled NPC (RC-NPC): an NPC driven by a queue of commands from
// the second player instead of the AI. See docs/mp/architecture-notes.md.
namespace mp
{

bool is_remote( const npc &guy );
// The same for any character: false for the avatar and AI-driven NPCs.
bool is_remote_character( const Character &who );
// Whether a second player drives this vehicle (hook in
// vehicle::player_is_driving_this_veh(): vehicles move only for their driver).
bool remote_drives( const map &here, const vehicle &veh );
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
// The remote NPC that network commands go to: the first one in the reality
// bubble. nullptr if there is none.
npc *network_npc();
bool has_commands( const npc &guy );

// Hook for npc::move(): runs queued commands instead of the AI.
// Returns false if guy is not remote-controlled.
bool remote_move( npc &guy );
// True if guy is remote-controlled and has nothing to do until a new
// command arrives. monmove() stops calling npc::move() for such NPCs and
// leaves them their remaining moves.
bool waits_for_commands( const npc &guy );

// How the second player's time counts while the server runs, chosen when
// hosting.
enum class time_mode : int {
    // Their actions take game time; whoever acts moves time on, and the one
    // standing still just waits (the host's avatar is made to wait while it
    // idles at the input).
    shared,
    // Their actions take no game time: the world never waits for them and
    // each command runs as soon as it arrives.
    instant,
};
// The server runs in time_mode::instant.
bool instant_mode();
// The server runs in time_mode::shared.
bool shared_time();
// Shared time: the character has spent its moves or has something going on
// (commands, an activity, sleep), so its new commands wait.
bool busy( const npc &guy );
// Shared time: the second player is owed game time, so a host idling at the
// input lets a turn pass.
bool remote_needs_time();
// Runs guy's queued commands and the activities they start right away.
void run_instantly( npc &guy );

// Hook at the start of do_turn(). While a remote NPC has moves left and no
// commands, game time stands still: this loop keeps the screen alive and lets
// the host use only actions that take no game time.
// host_input handles one host action and returns true if the game is over.
void wait_for_remote_players( const std::function<bool()> &host_input );
// Hook in game::handle_action(). Returns true if the action was taken over:
// while the world waits, movement, wait and pickup keys go to the waiting
// remote NPC (hot-seat control for testing), other actions that take time
// are refused.
bool intercept_host_action( action_id act );
// Hook in game::get_player_input(), called every 125 ms while the host's
// input waits for a key: serves the network, and returns true if the input
// should stop waiting, because the world waits for the second player and
// something arrived from the network.
bool host_input_should_yield();
// The timeout of the host's input loop, in milliseconds (hook in
// game::get_player_input()): short in shared time, so that the second
// player's steps don't wait long for an idle host.
int host_input_timeout();

// Hooks for npc::store() / npc::load() in savegame_json.cpp.
void store_npc( const npc &guy, JsonOut &json );
void load_npc( npc &guy, const JsonObject &data );

// Called by "Host game" in the main menu before the save is loaded: once the
// world is running, start the server and ask who the second player plays.
void request_host();

// Entry of the debug menu: toggle the flag, queue commands by hand.
void debug_menu();

} // namespace mp

#endif // CATA_SRC_MP_RC_NPC_H
