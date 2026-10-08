#pragma once
#ifndef CATA_SRC_MP_WORLD_SYNC_H
#define CATA_SRC_MP_WORLD_SYNC_H

#include "coordinates.h"

class JsonObject;
class JsonOut;
class npc;

// A copy of the map around the second player's character on the client, so
// that the game's own screens (picking up, looking around, aiming...) work
// there. The host sends submaps in the savegame format; the client puts them
// into its mapbuffer and loads its map around the character.
namespace mp::world_sync
{

// ---- Host ----

// Forget what was sent: a new client gets everything again.
void reset();
// Members of a "submaps" message with the submaps around guy that changed
// since they were last sent; false if none did.
bool write_changed( JsonOut &json, const npc &guy );

// ---- Client ----

void read( const JsonObject &message );
// Hook in map::loadn(): the client has no world to generate missing
// submaps from; puts empty ones there instead. True if it did.
bool fill_missing( const tripoint_abs_sm &omt_base );

} // namespace mp::world_sync

#endif // CATA_SRC_MP_WORLD_SYNC_H
