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
// Members of a "creatures" message: the monsters and NPCs guy sees (the
// host's avatar as an NPC); false if nothing changed since last time.
bool write_creatures( JsonOut &json, const npc &guy );
// Members of an "overmap" message: the overmap terrain around guy, for the
// sidebar and the minimap; false if unchanged.
bool write_overmap( JsonOut &json, const npc &guy );
// Members of a "world" message: time and weather.
void write_world( JsonOut &json );
// The character as the client loads it into its avatar; empty if unchanged
// since last time (or always, with force). Also empty, with `moved` set,
// when only its position changed: a step needs no whole character.
std::string character_if_changed( const npc &guy, bool force, bool *moved = nullptr );

// ---- Client ----

void read( const JsonObject &message );
void read_creatures( const JsonObject &message );
// A "position" message: the character stepped.
void read_position( const JsonObject &message );
void read_world( const JsonObject &message );
void read_overmap( const JsonObject &message );
// Keeps the client's map around its avatar (the copy of the character).
void follow_avatar();
// The game is left: the next host's map starts somewhere else.
void forget_host();
// Hook in map::loadn(): the client has no world to generate missing
// submaps from; puts empty ones there instead. True if it did.
bool fill_missing( const tripoint_abs_sm &omt_base );

} // namespace mp::world_sync

#endif // CATA_SRC_MP_WORLD_SYNC_H
