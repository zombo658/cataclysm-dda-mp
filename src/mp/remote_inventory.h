#pragma once
#ifndef CATA_SRC_MP_REMOTE_INVENTORY_H
#define CATA_SRC_MP_REMOTE_INVENTORY_H

#include <string>
#include <vector>

class JsonObject;
class JsonOut;
class npc;

// The second player's inventory: the host sends a numbered list of what the
// remote NPC holds, the client picks an item by its number and an action.
namespace mp::inventory
{

// ---- Host ----

// Writes the members of an "inventory" message (without "type") and
// remembers the items, so that the numbers can be resolved later.
void write( JsonOut &json, npc &guy );
// Carries out action on item index of the list with the given revision.
// Returns an empty string on success, otherwise why it can't be done.
std::string act( npc &guy, int revision, int index, const std::string &action );

// ---- Client ----

struct entry {
    int index = 0;
    std::string name;
    // "wielded", "worn", or the name of the container it is in.
    std::string where;
    // 0 for what the character holds or wears, 1 for its contents, ...
    int depth = 0;
    // Actions that make sense for the item: wield, unwield, wear, takeoff,
    // drop, eat.
    std::vector<std::string> actions;
};
struct listing {
    int revision = 0;
    std::vector<entry> entries;
};
listing read( const JsonObject &message );

} // namespace mp::inventory

#endif // CATA_SRC_MP_REMOTE_INVENTORY_H
