#pragma once
#ifndef CATA_SRC_MP_CRAFT_SCREEN_H
#define CATA_SRC_MP_CRAFT_SCREEN_H

#include <string>
#include <utility>
#include <vector>

#include "color.h"

class Character;
class recipe;
class recipe_subset;

// What the host's crafting screen computes about a recipe, for the second
// player's copy of that screen. Defined at the end of crafting_gui.cpp, next
// to the code they reuse.
namespace mp::craft_screen
{

struct recipe_state {
    bool can_craft = false;
    bool has_primary_skill = false;
    // In the list, highlighted in the list, and of the description.
    nc_color color = c_dark_gray;
    nc_color selected_color = h_dark_gray;
    nc_color info_color = c_dark_gray;
};

recipe_state state( Character &crafter, const recipe &r, int batch );
// The description to the right of the list, folded to fold_width.
std::vector<std::string> info( Character &crafter, const recipe &r, int batch, int fold_width );
// The crafting speed line at the top right.
std::pair<nc_color, std::string> speed_indicator( Character &crafter, const recipe &r );
// The panel about the result on wide screens, as text with color tags.
std::string result_info( Character &crafter, const recipe &r, int batch, int width );
// The recipes matching a search of the crafting screen ("c:plank" and so on).
std::vector<const recipe *> filter( const recipe_subset &recipes, const std::string &query,
                                    const Character &crafter );
// The help shown when typing a search.
std::string filter_help();
// The categories of the tabs, in order.
std::vector<std::string> categories();
// Category and subcategory names as the tabs show them, untranslated.
std::string category_name( const std::string &category );
std::string subcategory_name( const std::string &category, const std::string &subcategory );

} // namespace mp::craft_screen

#endif // CATA_SRC_MP_CRAFT_SCREEN_H
