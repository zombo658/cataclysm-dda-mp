#pragma once
#ifndef CATA_SRC_MP_REMOTE_CRAFTING_H
#define CATA_SRC_MP_REMOTE_CRAFTING_H

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "color.h"
#include "mp/craft_screen.h"

class JsonObject;
class JsonOut;
class npc;

// Crafting for the second player. The client shows a copy of the host's
// crafting screen; everything that depends on the character (what can be
// made, what is missing, how fast) is computed on the host by the same code
// as the host's own screen and sent over.
namespace mp::remote_crafting
{

// ---- Host ----

// Members of the answers (without "type"); see docs/mp/protocol.md.
void write_recipes( JsonOut &json, npc &guy );
void write_states( JsonOut &json, npc &guy, const JsonObject &request );
void write_info( JsonOut &json, npc &guy, const JsonObject &request );
void write_filter( JsonOut &json, npc &guy, const JsonObject &request );
// Starts crafting; returns an empty string or why it can't be done.
std::string start( npc &guy, const std::string &recipe, int batch );

// ---- Client ----

struct description {
    std::vector<std::string> lines;
    nc_color indicator_color = c_white;
    std::string indicator;
    // About the result, for the panel on wide screens.
    std::string result;
};

// What the host has answered so far.
struct client_cache {
    std::optional<std::vector<std::string>> recipes;
    std::map<std::pair<std::string, int>, craft_screen::recipe_state> states;
    // By recipe, batch size, width of the description and of the result panel.
    std::map<std::tuple<std::string, int, int, int>, description> infos;
    std::map<std::string, std::vector<std::string>> filters;
    // Asked and not answered yet, not to ask again.
    std::set<std::tuple<std::string, int, int, int>> infos_asked;
};

// Takes the answers above from the host; false for other messages.
bool read_message( client_cache &cache, const std::string &type, const JsonObject &message );

// The crafting screen. poll() reads the network (answers end up in cache)
// and returns false when the connection is lost.
void show_screen( client_cache &cache, const std::function<bool()> &poll );

} // namespace mp::remote_crafting

#endif // CATA_SRC_MP_REMOTE_CRAFTING_H
