#pragma once
#ifndef CATA_SRC_MP_NPC_STEP_H
#define CATA_SRC_MP_NPC_STEP_H

#include <string>
#include <vector>

#include "coordinates.h"

class npc;

// A step of the second player's character the way the host's avatar steps
// (avatar_action::move() and game::walk_move()), not the AI's npc::move_to():
// no side-stepping of fields, no shoving of NPCs, questions about dangerous
// tiles and the menu for a friendly NPC go to the second player.
namespace mp::npc_step
{

// game::get_dangerous_tile() for guy instead of the avatar.
std::vector<std::string> dangerous_tile( const npc &guy, const tripoint_bub_ms &dest );

// Returns false if the step is not one this handles (z-level moves through
// ramps, being stunned...): the caller uses npc::move_to() then.
bool step( npc &guy, const tripoint_rel_ms &d );

} // namespace mp::npc_step

#endif // CATA_SRC_MP_NPC_STEP_H
