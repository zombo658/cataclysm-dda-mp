#include "mp/client_ui.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "action.h"
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
    // Which action the inventory was asked for ("" - any).
    std::string inventory_filter;
    // The sidebar lines from the host, with color tags.
    std::vector<std::string> sidebar;
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
        } else if( type == "sidebar" ) {
            state.sidebar.clear();
            for( const std::string line : msg.get_array( "lines" ) ) {
                state.sidebar.push_back( line );
            }
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

// Item list, then what to do with the chosen item. With a filter (the host's
// "wear", "eat", ... keys) only items that allow it are listed, and choosing
// one does it.
void show_inventory( const inventory::listing &inv, const std::string &filter )
{
    std::vector<const inventory::entry *> shown;
    for( const inventory::entry &e : inv.entries ) {
        if( filter.empty() || std::find( e.actions.begin(), e.actions.end(), filter ) != e.actions.end() ) {
            shown.push_back( &e );
        }
    }
    if( shown.empty() ) {
        popup( filter.empty() ? _( "You have nothing." ) : _( "You have nothing for that." ) );
        return;
    }
    uilist items;
    items.text = filter.empty() ? _( "Inventory" ) : action_name( filter );
    for( size_t i = 0; i < shown.size(); i++ ) {
        const inventory::entry &e = *shown[i];
        const std::string indent( filter.empty() ? e.depth * 2 : 0, ' ' );
        const std::string where = e.depth == 0 ? string_format( " <color_dark_gray>(%s)</color>",
                                  e.where == "wielded" ? _( "in hands" ) : e.where == "worn" ? _( "worn" ) : _( "carried" ) ) :
                                  filter.empty() ? "" : string_format( " <color_dark_gray>(%s)</color>", e.where );
        items.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, indent + e.name + where );
    }
    items.query();
    if( items.ret < 0 || static_cast<size_t>( items.ret ) >= shown.size() ) {
        return;
    }
    const inventory::entry &chosen = *shown[items.ret];
    if( !filter.empty() ) {
        send_item_command( inv.revision, chosen.index, filter );
        return;
    }
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
    if( cmd != "status" && cmd != "inventory" ) {
        state.my_turn = false;
    }
}

std::string direction_name( const point_rel_ms &d )
{
    static const std::map<std::pair<int, int>, std::string> names = {
        { { 0, -1 }, "n" }, { { 1, -1 }, "ne" }, { { 1, 0 }, "e" }, { { 1, 1 }, "se" },
        { { 0, 1 }, "s" }, { { -1, 1 }, "sw" }, { { -1, 0 }, "w" }, { { -1, -1 }, "nw" },
    };
    const auto it = names.find( { d.x(), d.y() } );
    return it == names.end() ? "" : it->second;
}

// The whole log, newest last.
void show_messages( const client_state &state )
{
    std::string text;
    const size_t first = state.log.size() > 60 ? state.log.size() - 60 : 0;
    for( size_t i = first; i < state.log.size(); i++ ) {
        text += colorize( state.log[i].first, state.log[i].second ) + "\n";
    }
    popup( text.empty() ? std::string( _( "No messages." ) ) : text );
}

// The host's keys, as the game's actions. Returns false when the player
// wants to leave.
bool handle_action( client_state &state, const std::string &action, const input_context &ctxt )
{
    const action_id act = look_up_action( action );
    switch( act ) {
        case ACTION_MOVE_FORTH:
        case ACTION_MOVE_FORTH_RIGHT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_BACK_RIGHT:
        case ACTION_MOVE_BACK:
        case ACTION_MOVE_BACK_LEFT:
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_FORTH_LEFT:
            send_command( state, "move",
                          direction_name( get_delta_from_movement_action( act, iso_rotate::yes ) ) );
            break;
        case ACTION_PAUSE:
            send_command( state, "wait" );
            break;
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL:
            send_command( state, "pickup" );
            break;
        case ACTION_INVENTORY:
            state.inventory_filter.clear();
            send_command( state, "inventory" );
            break;
        case ACTION_WIELD:
            state.inventory_filter = "wield";
            send_command( state, "inventory" );
            break;
        case ACTION_WEAR:
            state.inventory_filter = "wear";
            send_command( state, "inventory" );
            break;
        case ACTION_TAKE_OFF:
            state.inventory_filter = "takeoff";
            send_command( state, "inventory" );
            break;
        case ACTION_EAT:
            state.inventory_filter = "eat";
            send_command( state, "inventory" );
            break;
        case ACTION_DROP:
            state.inventory_filter = "drop";
            send_command( state, "inventory" );
            break;
        case ACTION_MESSAGES:
            show_messages( state );
            break;
        case ACTION_MAIN_MENU:
            return !query_yn( _( "Leave the game?" ) );
        case ACTION_NULL:
        case ACTION_TIMEOUT:
            break;
        default:
            state.add_log( string_format( _( "%s: not available to the second player yet." ),
                                          ctxt.get_action_name( action ) ), c_dark_gray );
            break;
    }
    return true;
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

// Width of the sidebar, like the host's.
int sidebar_width( const int screen_width )
{
    return screen_width >= 110 ? 55 : std::max( 32, screen_width / 3 );
}

// Right: the sidebar from the host, then the log, newest at the bottom.
void draw_sidebar( const catacurses::window &w, const client_state &state )
{
    werase( w );
    const int width = getmaxx( w );
    const int height = getmaxy( w );
    int y = 0;
    const std::string connection = state.lost ? colorize( _( "Disconnected" ), c_red ) :
                                   state.instant || state.my_turn ? colorize( state.character, c_light_green ) :
                                   colorize( _( "Waiting for the host…" ), c_yellow );
    trim_and_print( w, point( 0, y++ ), width, c_white, connection );
    for( const std::string &line : state.sidebar ) {
        if( y >= height ) {
            break;
        }
        trim_and_print( w, point( 0, y++ ), width, c_white, line );
    }
    y++;
    // The log fills the rest, wrapped, newest at the bottom.
    std::vector<std::pair<std::string, nc_color>> wrapped;
    for( auto it = state.log.rbegin(); it != state.log.rend() &&
         static_cast<int>( wrapped.size() ) < height; ++it ) {
        const std::vector<std::string> folded = foldstring( it->first, width );
        for( auto f = folded.rbegin(); f != folded.rend(); ++f ) {
            wrapped.emplace_back( *f, it->second );
        }
    }
    const int room = std::max( 0, height - y );
    const int count = std::min<int>( room, wrapped.size() );
    for( int i = 0; i < count; i++ ) {
        const auto &line = wrapped[count - 1 - i];
        trim_and_print( w, point( 0, y + i ), width, line.second, line.first );
    }
    wnoutrefresh( w );
}

// Left: the map, the character in the middle.
void draw_map( const catacurses::window &w_map, const client_state &state )
{
    werase( w_map );
    const view::grid &grid = state.grid;
#if defined(TILES)
    if( use_tiles && tilecontext && state.data_loaded ) {
        // The window goes to the screen first: the tiles are drawn over it.
        wnoutrefresh( w_map );
        const window_dimensions dim = get_window_dimensions( w_map );
        tilecontext->draw_remote_view( dim.window_pos_pixel, dim.window_size_pixel.x,
                                       dim.window_size_pixel.y, grid );
        return;
    }
#endif
    const int width = getmaxx( w_map );
    const int height = getmaxy( w_map );
    const int size = static_cast<int>( grid.rows.size() );
    const point screen_center( width / 2, height / 2 );
    for( int sy = 0; sy < height; sy++ ) {
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
            mvwprintz( w_map, point( sx, sy ), color, c.symbol );
        }
    }
    wnoutrefresh( w_map );
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
    // The same layout as the host's screen: the map on the left, the sidebar
    // and the log on the right.
    catacurses::window w_map;
    catacurses::window w_side;
    ui_adaptor ui;
    ui.on_screen_resize( [&]( ui_adaptor & ui ) {
        const int side = sidebar_width( TERMX );
        w_map = catacurses::newwin( TERMY, TERMX - side, point::zero );
        w_side = catacurses::newwin( TERMY, side, point( TERMX - side, 0 ) );
        ui.position( point::zero, point( TERMX, TERMY ) );
    } );
    ui.mark_resize();
    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw_sidebar( w_side, state );
        draw_map( w_map, state );
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

    // The host's own keys.
    input_context ctxt = get_default_mode_input_context();
    ctxt.set_timeout( 100 );
    while( true ) {
        net::client_poll( h );
        if( state.data_requested ) {
            load_host_data( state );
            ui.mark_resize();
        }
        if( state.inventory_to_show ) {
            const inventory::listing inv = *state.inventory_to_show;
            state.inventory_to_show.reset();
            show_inventory( inv, state.inventory_filter );
            ui.invalidate_ui();
        }
        ui_manager::redraw();
        const std::string action = ctxt.handle_input();
        if( action == "TIMEOUT" ) {
            continue;
        }
        if( state.lost ) {
            const int ch = ctxt.get_raw_input().get_first_input();
            if( ch == 'q' || ch == KEY_ESCAPE || ch == '\n' ) {
                break;
            }
            continue;
        }
        if( !handle_action( state, action, ctxt ) ) {
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
