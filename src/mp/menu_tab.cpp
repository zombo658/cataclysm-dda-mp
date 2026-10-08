#include "mp/menu_tab.h"

#include <optional>
#include <string>
#include <vector>

#include "output.h"
#include "translations.h"
#include "uilist.h"
#include "worldfactory.h"

namespace mp
{

const std::vector<std::string> &main_menu_items()
{
    // Rebuilt every time: the language can change in the options.
    static std::vector<std::string> items;
    items = { _( "Host game" ), _( "Join game" ) };
    return items;
}

std::optional<std::string> pick_world_to_host()
{
    const std::vector<std::string> worlds = world_generator->all_worldnames();
    if( worlds.empty() ) {
        popup( _( "Create a world and a character first (New Game), then host it." ) );
        return std::nullopt;
    }
    uilist menu;
    menu.text = _( "Which world to host?" );
    for( size_t i = 0; i < worlds.size(); i++ ) {
        menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, worlds[i] );
    }
    menu.query();
    if( menu.ret < 0 || static_cast<size_t>( menu.ret ) >= worlds.size() ) {
        return std::nullopt;
    }
    return worlds[menu.ret];
}

} // namespace mp
