#pragma once
#ifndef CATA_SRC_MP_WORLD_ACTIONS_H
#define CATA_SRC_MP_WORLD_ACTIONS_H

#include <string>

#include "action.h"
#include "coordinates.h"

class npc;

// Actions on a tile next to the second player's character: open, close,
// smash, examine. The host's versions (handle_action.cpp, game::examine())
// work on the avatar; these do the same for the remote character. What the
// game asks meanwhile goes to the client (mp/remote_prompt.h).
namespace mp::world_actions
{

// ---- Host ----

// Returns an empty string or why it can't be done.
std::string act( npc &guy, const std::string &action, const tripoint_rel_ms &dir );

// ---- Client ----

// Asks the direction like the host's game and sends the command; false if
// the action is not one of these.
bool run( action_id act );

} // namespace mp::world_actions

#endif // CATA_SRC_MP_WORLD_ACTIONS_H
