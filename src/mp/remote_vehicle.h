#pragma once
#ifndef CATA_SRC_MP_REMOTE_VEHICLE_H
#define CATA_SRC_MP_REMOTE_VEHICLE_H

#include <optional>
#include <string>

#include "coordinates.h"

class Character;
class JsonObject;
class npc;
class vehicle;

// The vehicle screen (veh_interact) of the second player runs on the
// client's copy of the vehicle. Installing, repairing, removing... are
// activities and reach the host as such; the few things the screen changes
// right away (name, labels, crew, shape of a part, unloading fuel) are sent
// as "vehicle_edit" commands by hooks in veh_interact.cpp and veh_shape.cpp.
namespace mp::remote_vehicle
{

// ---- Client (hooks; they do nothing for the host's own screen) ----

void renamed( const vehicle &veh );
void relabeled( const vehicle &veh, const point_rel_ms &mount, const std::string &label );
void crew_changed( const vehicle &veh, int part, int npc_id );
void shape_changed( const vehicle &veh, int part, const std::string &variant );
// True if the client sent it (the host unloads into the second player's hands).
bool fuel_unloaded( const vehicle &veh );
// A "vehicle" question: the host finished work on the vehicle, its screen
// opens again as it does for the host.
void reopen( const JsonObject &question );

// ---- Host ----

std::string edit( npc &guy, const JsonObject &request );
// Hook in activity_handlers::vehicle_finish(): the second player's screen
// opens again.
void work_done( const Character &who, const vehicle &veh, const point_rel_ms &int_p );
// Hook in orient_part(): the facing of a new light is the second player's
// choice; std::nullopt when it's the host's.
std::optional<std::optional<point_rel_ms>> ask_facing( const std::string &part_name );

} // namespace mp::remote_vehicle

#endif // CATA_SRC_MP_REMOTE_VEHICLE_H
