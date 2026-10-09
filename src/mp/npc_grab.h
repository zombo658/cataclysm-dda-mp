#pragma once
#ifndef CATA_SRC_MP_NPC_GRAB_H
#define CATA_SRC_MP_NPC_GRAB_H

#include <string>

#include "coordinates.h"
#include "enums.h"

class npc;

// Grabbing and dragging furniture and vehicles for the second player's
// character. The host's code (handle_action.cpp grab(), game::walk_move(),
// game::grabbed_furn_move(), game::grabbed_veh_move()) works on the avatar
// only, whose grab type lives in `avatar`; these are copies for an npc.
// The grab point is Character::grab_point, the type is kept here.
namespace mp::npc_grab
{

object_type type( const npc &guy );

// The "grab" tile action: grabs what is at `dir`, or lets go.
std::string toggle( npc &guy, const tripoint_rel_ms &dir );

// Before a step by `dp`. `may_enter` is set when the step goes into a tile
// the grabbed thing leaves (pushing) or the character does not move
// (shifting furniture), so the caller must not reject it as blocked.
void prepare_step( npc &guy, const tripoint_rel_ms &dp, bool &may_enter );

// Moves the grabbed thing; true if the character must not step.
bool drag( npc &guy, const tripoint_rel_ms &dp );

// Adds the grab to the character's JSON for the client's avatar.
void add_to_json( std::string &data, const npc &guy );

} // namespace mp::npc_grab

#endif // CATA_SRC_MP_NPC_GRAB_H
