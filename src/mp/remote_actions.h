#pragma once
#ifndef CATA_SRC_MP_REMOTE_ACTIONS_H
#define CATA_SRC_MP_REMOTE_ACTIONS_H

#include <string>

#include "action.h"

class JsonObject;
class item_location;
class npc;

// The host's own screens on the second player's side. The client keeps a
// copy of the remote character in its (otherwise unused) avatar and opens
// the very screens the host's game opens for an action. Where such a screen
// would do something, a hook sends what was chosen to the host, which does
// it to the real character.
namespace mp::remote_actions
{

// ---- Host ----

// {"cmd":"item_action"}: what the item menu of the inventory chose
// ("key" is the menu's hotkey). Returns an empty string or why not.
std::string item_action( npc &guy, const JsonObject &request );
// {"cmd":"combat"}: firing or throwing at a target chosen on the client
// with the game's own aiming screen.
std::string combat( npc &guy, const JsonObject &request );

// ---- Client ----

// Set while the client screen runs: the hooks below only act then.
void set_client_active( bool active );
bool client_active();

// Whether the action is done this way (and needs the character first).
bool uses_character( action_id act );
// Loads the character sent by the host into the client's avatar.
bool load_character( const std::string &data );
// Opens the action's screen on the loaded character.
void run( action_id act );

// Hook in game::inventory_item_menu(): sends the chosen action to the host.
// True when the menu must close (the copy is out of date now).
bool forward_item_action( const item_location &loc, int key );

} // namespace mp::remote_actions

#endif // CATA_SRC_MP_REMOTE_ACTIONS_H
