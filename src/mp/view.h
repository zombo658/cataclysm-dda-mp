#pragma once
#ifndef CATA_SRC_MP_VIEW_H
#define CATA_SRC_MP_VIEW_H

#include <string>
#include <vector>

class JsonObject;
class JsonOut;
class npc;

// What the remote NPC sees around itself, flattened to one symbol and one
// color per tile, so that the client can draw it without loading the game
// data. Format: docs/mp/protocol.md, message "view".
namespace mp::view
{

constexpr int radius = 25;

// Writes the members of a "view" message (without "type").
void write( JsonOut &json, const npc &guy );

// Client side: a decoded view.
struct cell {
    std::string symbol;
    std::string color;
};
struct grid {
    int radius = 0;
    // (2 * radius + 1) rows of (2 * radius + 1) cells; the NPC is in the middle.
    std::vector<std::vector<cell>> rows;
};
grid read( const JsonObject &message );

} // namespace mp::view

#endif // CATA_SRC_MP_VIEW_H
