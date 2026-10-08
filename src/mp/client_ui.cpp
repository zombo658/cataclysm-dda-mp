#include "mp/client_ui.h"

#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "color.h"
#include "debug.h"
#include "game.h"
#include "init.h"
#include "loading_ui.h"
#include "mod_manager.h"
#include "type_id.h"
#include "cursesdef.h"
#include "input.h"
#include "input_context.h"
#include "json.h"
#include "json_loader.h"
#include "mp/net.h"
#include "mp/protocol.h"
#include "mp/remote_inventory.h"
#include "mp/sound_relay.h"
#include "mp/view.h"
#include "output.h"
#include "point.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"
#include "worldfactory.h"

#if defined(TILES)
#include "cached_options.h"
#include "cata_tiles.h"
#include "sdltiles.h"
#endif

namespace mp::client_ui
{

namespace
{

// Address typed last time, offered again.
std::string last_address = "127.0.0.1";

struct client_state {
    view::grid grid;
    std::string character;
    std::string status;
    bool my_turn = false;
    // The host runs with instant actions: no turns to wait for.
    bool instant = false;
    bool attack_pending = false;
    bool lost = false;
    // Mods of the host's world; the client loads their data to draw tiles.
    std::vector<std::string> mods;
    bool data_requested = false;
    bool data_loaded = false;
    // The game's loading code expects an active world (mod list, options):
    // a world in memory only, with the host's mods.
    std::unique_ptr<WORLD> host_world;
    // The inventory came; show it on the next round of the main loop.
    std::optional<inventory::listing> inventory_to_show;
    std::deque<std::pair<std::string, nc_color>> log;

    void add_log( const std::string &text, const nc_color &color = c_light_gray ) {
        std::istringstream lines( text );
        std::string line;
        while( std::getline( lines, line ) ) {
            log.emplace_back( line, color );
        }
        while( log.size() > 200 ) {
            log.pop_front();
        }
    }
};

std::string format_status( const JsonObject &status )
{
    status.allow_omitted_members();
    return string_format( _( "%1$s  HP %2$d/%3$d  Stamina %4$d/%5$d  Hunger %6$d  Thirst %7$d  "
                             "Sleepiness %8$d  Pain %9$d" ),
                          status.get_string( "name", "" ), status.get_int( "hp", 0 ),
                          status.get_int( "hp_max", 0 ), status.get_int( "stamina", 0 ),
                          status.get_int( "stamina_max", 0 ), status.get_int( "hunger", 0 ),
                          status.get_int( "thirst", 0 ), status.get_int( "sleepiness", 0 ),
                          status.get_int( "pain", 0 ) );
}

void handle_message( client_state &state, const std::string &line )
{
    try {
        const JsonValue value = json_loader::from_string( line );
        const JsonObject msg = value.get_object();
        msg.allow_omitted_members();
        const std::string type = msg.get_string( "type", "" );
        if( type == "view" ) {
            state.grid = view::read( msg );
        } else if( type == "your_turn" || type == "status" || type == "state" ) {
            state.status = format_status( msg.get_object( "status" ) );
            if( type == "your_turn" ) {
                state.my_turn = true;
            }
        } else if( type == "welcome" ) {
            state.character = msg.get_string( "npc", "" );
            state.instant = msg.get_bool( "instant", false );
            if( msg.has_array( "mods" ) ) {
                for( const std::string mod : msg.get_array( "mods" ) ) {
                    state.mods.push_back( mod );
                }
#if defined(TILES)
                // Only tiles need the data; the text map comes ready to draw.
                state.data_requested = use_tiles;
#endif
            }
            if( msg.get_int( "version", 0 ) != protocol::version ) {
                state.add_log( _( "Warning: the host runs a different version of the game." ), c_yellow );
            }
            state.add_log( state.character.empty() ?
                           _( "Connected.  The host hasn't chosen your character yet." ) :
                           string_format( _( "Connected.  You play %s." ), state.character ), c_light_green );
        } else if( type == "log" ) {
            for( const std::string text : msg.get_array( "lines" ) ) {
                state.add_log( text );
            }
        } else if( type == "rejected" ) {
            state.add_log( string_format( _( "Can't do that: %s" ), msg.get_string( "reason", "" ) ),
                           c_light_red );
        } else if( type == "inventory" ) {
            state.inventory_to_show = inventory::read( msg );
        } else if( type == "sfx" ) {
            sound_relay::play( msg );
        } else if( type == "error" ) {
            state.add_log( msg.get_string( "message", "" ), c_red );
        }
    } catch( const JsonError &err ) {
        state.add_log( string_format( _( "Bad message from the host: %s" ), err.what() ), c_red );
    }
}

void send_item_command( const int revision, const int index, const std::string &action )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", "item" );
    json.member( "revision", revision );
    json.member( "index", index );
    json.member( "action", action );
    json.end_object();
    net::client_send_line( os.str() );
}

std::string action_name( const std::string &action )
{
    if( action == "wield" ) {
        return _( "Wield" );
    } else if( action == "unwield" ) {
        return _( "Put away" );
    } else if( action == "wear" ) {
        return _( "Wear" );
    } else if( action == "takeoff" ) {
        return _( "Take off" );
    } else if( action == "eat" ) {
        return _( "Eat / drink" );
    } else if( action == "drop" ) {
        return _( "Drop" );
    }
    return action;
}

// Item list, then what to do with the chosen item.
void show_inventory( const inventory::listing &inv )
{
    if( inv.entries.empty() ) {
        popup( _( "You have nothing." ) );
        return;
    }
    uilist items;
    items.text = _( "Inventory" );
    for( size_t i = 0; i < inv.entries.size(); i++ ) {
        const inventory::entry &e = inv.entries[i];
        const std::string indent( e.depth * 2, ' ' );
        const std::string where = e.depth == 0 ? string_format( " <color_dark_gray>(%s)</color>",
                                  e.where == "wielded" ? _( "in hands" ) : e.where == "worn" ? _( "worn" ) : _( "carried" ) ) : "";
        items.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, indent + e.name + where );
    }
    items.query();
    if( items.ret < 0 || static_cast<size_t>( items.ret ) >= inv.entries.size() ) {
        return;
    }
    const inventory::entry &chosen = inv.entries[items.ret];
    uilist actions;
    actions.text = chosen.name;
    for( size_t i = 0; i < chosen.actions.size(); i++ ) {
        actions.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, action_name( chosen.actions[i] ) );
    }
    actions.query();
    if( actions.ret < 0 || static_cast<size_t>( actions.ret ) >= chosen.actions.size() ) {
        return;
    }
    send_item_command( inv.revision, chosen.index, chosen.actions[actions.ret] );
}

void send_command( client_state &state, const std::string &cmd, const std::string &dir = "" )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", cmd );
    if( !dir.empty() ) {
        json.member( "dir", dir );
    }
    json.end_object();
    net::client_send_line( os.str() );
    if( cmd != "status" ) {
        state.my_turn = false;
    }
}

std::optional<std::string> key_direction( const int ch )
{
    static const std::map<int, std::string> dirs = {
        { 'k', "n" }, { 'u', "ne" }, { 'l', "e" }, { 'n', "se" },
        { 'j', "s" }, { 'b', "sw" }, { 'h', "w" }, { 'y', "nw" },
        { '8', "n" }, { '9', "ne" }, { '6', "e" }, { '3', "se" },
        { '2', "s" }, { '1', "sw" }, { '4', "w" }, { '7', "nw" },
        { KEY_UP, "n" }, { KEY_RIGHT, "e" }, { KEY_DOWN, "s" }, { KEY_LEFT, "w" },
        { KEY_HOME, "nw" }, { KEY_PPAGE, "ne" }, { KEY_END, "sw" }, { KEY_NPAGE, "se" },
    };
    const auto it = dirs.find( ch );
    if( it == dirs.end() ) {
        return std::nullopt;
    }
    return it->second;
}

// Returns false when the player wants to leave.
bool handle_key( client_state &state, const int ch )
{
    const std::optional<std::string> dir = key_direction( ch );
    if( state.attack_pending ) {
        state.attack_pending = false;
        if( dir ) {
            send_command( state, "attack", *dir );
        } else {
            state.add_log( _( "Attack cancelled." ) );
        }
    } else if( dir ) {
        send_command( state, "move", *dir );
    } else if( ch == 'a' ) {
        state.attack_pending = true;
        state.add_log( _( "Attack in which direction?" ), c_yellow );
    } else if( ch == '.' || ch == '5' ) {
        send_command( state, "wait" );
    } else if( ch == 'g' || ch == ',' ) {
        send_command( state, "pickup" );
    } else if( ch == 's' ) {
        send_command( state, "status" );
    } else if( ch == 'i' ) {
        send_command( state, "inventory" );
    } else if( ch == 'q' || ch == KEY_ESCAPE ) {
        return !query_yn( _( "Leave the game?" ) );
    }
    return true;
}

constexpr int log_height = 8;
constexpr int map_top = 2;

int map_height( const int screen_height )
{
    return std::max( 1, screen_height - map_top - log_height - 1 );
}

// Bottom of the screen: the log and the keys.
void draw_log_and_keys( const catacurses::window &w, const client_state &state )
{
    const int width = getmaxx( w );
    const int height = getmaxy( w );
    const int log_top = height - log_height - 1;
    const int shown = std::min<int>( log_height, state.log.size() );
    for( int i = 0; i < shown; i++ ) {
        const auto &entry = state.log[state.log.size() - shown + i];
        trim_and_print( w, point( 0, log_top + i ), width, entry.second, entry.first );
    }
    trim_and_print( w, point( 0, height - 1 ), width, c_dark_gray,
                    _( "hjklyubn/arrows/numpad move  a+dir attack  . wait  g pick up  i inventory  s status  q leave" ) );
}

// Loads the data of the host's mods, so that tiles can be looked up the way
// the host's game does it.
void load_host_data( client_state &state )
{
    state.data_requested = false;
    std::vector<mod_id> mods;
    for( const std::string &name : state.mods ) {
        const mod_id mod( name );
        if( mod.is_valid() ) {
            mods.push_back( mod );
        } else {
            state.add_log( string_format( _( "You don't have the mod %s; some things will look odd." ),
                                          name ), c_yellow );
        }
    }
    state.host_world = std::make_unique<WORLD>();
    state.host_world->active_mod_order = mods;
    world_generator->set_active_world( state.host_world.get() );
    try {
        g->load_core_data();
        g->load_packs( _( "Loading the host's game data" ), mods );
        DynamicDataLoader::get_instance().finalize_loaded_data();
        state.data_loaded = true;
        state.add_log( _( "Loaded the host's game data." ), c_light_gray );
    } catch( const std::exception &err ) {
        state.add_log( string_format( _( "Can't load the game data: %s" ), err.what() ), c_red );
    }
    loading_ui::done();
}

void draw( const catacurses::window &w, [[maybe_unused]] const catacurses::window &w_map,
           const client_state &state )
{
    werase( w );
    const int width = getmaxx( w );
    const int height = getmaxy( w );
    const int map_height = mp::client_ui::map_height( height );

    // Top: who we are and whose turn it is.
    const bool can_act = state.instant || state.my_turn;
    const std::string turn = state.lost ? _( "Disconnected" ) :
                             state.instant ? _( "Connected" ) :
                             state.my_turn ? _( "YOUR TURN" ) : _( "Waiting for the host…" );
    mvwprintz( w, point( 0, 0 ), can_act && !state.lost ? c_light_green : c_yellow, turn );
    mvwprintz( w, point( utf8_width( turn ) + 2, 0 ), c_white, state.status );

    // Middle: the map, the character in the middle.
    const view::grid &grid = state.grid;
#if defined(TILES)
    if( use_tiles && tilecontext && state.data_loaded ) {
        // The text window has to be on the screen first: tiles go over it.
        draw_log_and_keys( w, state );
        wnoutrefresh( w );
        // A real window: get_window_dimensions() of a bare rectangle takes it
        // for the game's (here absent) terrain window and returns no size.
        const window_dimensions dim = get_window_dimensions( w_map );
        tilecontext->draw_remote_view( dim.window_pos_pixel, dim.window_size_pixel.x,
                                       dim.window_size_pixel.y, grid );
        return;
    }
#endif
    const int size = static_cast<int>( grid.rows.size() );
    const point screen_center( width / 2, map_top + map_height / 2 );
    for( int sy = map_top; sy < map_top + map_height; sy++ ) {
        const int row = grid.radius + ( sy - screen_center.y );
        if( row < 0 || row >= size ) {
            continue;
        }
        const std::vector<view::cell> &cells = grid.rows[row];
        for( int sx = 0; sx < width; sx++ ) {
            const int col = grid.radius + ( sx - screen_center.x );
            if( col < 0 || col >= static_cast<int>( cells.size() ) ) {
                continue;
            }
            const view::cell &c = cells[col];
            if( c.symbol == " " || c.symbol.empty() ) {
                continue;
            }
            const nc_color color = get_all_colors().name_to_color( c.color, report_color_error::no );
            mvwprintz( w, point( sx, sy ), color, c.symbol );
        }
    }

    draw_log_and_keys( w, state );
    wnoutrefresh( w );
}

} // namespace

void run_join_screen()
{
    string_input_popup address_popup;
    address_popup.title( _( "Host address (IP, or IP:port):" ) )
    .width( 40 )
    .text( last_address );
    const std::string address = address_popup.query_string();
    if( address_popup.canceled() || address.empty() ) {
        return;
    }
    last_address = address;

    std::string host = address;
    int port = net::default_port;
    const size_t colon = address.rfind( ':' );
    if( colon != std::string::npos ) {
        host = address.substr( 0, colon );
        port = std::atoi( address.substr( colon + 1 ).c_str() );
    }

    std::string error;
    {
        static_popup connecting;
        connecting.message( _( "Connecting to %s…" ), address );
        ui_manager::redraw();
        refresh_display();
        if( !net::connect_to( host, port, 5000, error ) ) {
            popup( _( "Can't connect to %1$s: %2$s" ), address, error );
            return;
        }
    }

    client_state state;
    catacurses::window w;
    // Where the tiles go: between the status line and the log.
    catacurses::window w_map;
    ui_adaptor ui;
    ui.on_screen_resize( [&]( ui_adaptor & ui ) {
        w = catacurses::newwin( TERMY, TERMX, point::zero );
        w_map = catacurses::newwin( map_height( TERMY ), TERMX, point( 0, map_top ) );
        ui.position_from_window( w );
    } );
    ui.mark_resize();
    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw( w, w_map, state );
    } );

    net::handlers h;
    h.on_line = [&]( const std::string & line ) {
        handle_message( state, line );
        ui.invalidate_ui();
    };
    h.on_disconnect = [&]() {
        state.lost = true;
        state.my_turn = false;
        state.add_log( _( "The connection to the host is lost." ), c_red );
        ui.invalidate_ui();
    };

    input_context ctxt( "MP_CLIENT" );
    ctxt.register_action( "ANY_INPUT" );
    ctxt.set_timeout( 100 );
    while( true ) {
        net::client_poll( h );
        if( state.data_requested ) {
            load_host_data( state );
            ui.invalidate_ui();
        }
        if( state.inventory_to_show ) {
            const inventory::listing inv = *state.inventory_to_show;
            state.inventory_to_show.reset();
            show_inventory( inv );
            ui.invalidate_ui();
        }
        ui_manager::redraw();
        const std::string action = ctxt.handle_input();
        if( action == "TIMEOUT" ) {
            continue;
        }
        const int ch = ctxt.get_raw_input().get_first_input();
        if( state.lost ) {
            if( ch == 'q' || ch == KEY_ESCAPE || ch == '\n' ) {
                break;
            }
            continue;
        }
        if( !handle_key( state, ch ) ) {
            break;
        }
        ui.invalidate_ui();
    }
    net::disconnect();
    if( state.host_world ) {
        // Back to what the main menu expects: core data only, no world.
        try {
            g->load_core_data();
        } catch( const std::exception &err ) {
            debugmsg( "Can't reload the core data: %s", err.what() );
        }
        world_generator->set_active_world( nullptr );
    }
}

} // namespace mp::client_ui
