#pragma once
#ifndef CATA_SRC_MP_MENU_TAB_H
#define CATA_SRC_MP_MENU_TAB_H

#include <optional>
#include <string>
#include <vector>

// The "Multiplayer" tab of the main menu (src/main_menu.cpp).
namespace mp
{

// Entries of the tab: 0 - host a game, 1 - join a game.
const std::vector<std::string> &main_menu_items();

// Asks which world to host. nullopt if cancelled or there are no worlds.
std::optional<std::string> pick_world_to_host();

} // namespace mp

#endif // CATA_SRC_MP_MENU_TAB_H
