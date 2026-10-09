#include "mp/client_ui.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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
#include "avatar.h"
#include "json.h"
#include "map.h"
#include "mapbuffer.h"
#include "messages.h"
#include "calendar.h"
#include "weather.h"
#include "options.h"
#include "panels.h"
#include "json_loader.h"
#include "mp/net.h"
#include "mp/player_talk.h"
#include "mp/protocol.h"
#include "mp/remote_actions.h"
#include "mp/remote_crafting.h"
#include "mp/remote_log.h"
#include "mp/remote_prompt.h"
#include "mp/sound_relay.h"
#include "mp/view.h"
#include "mp/world_actions.h"
#include "mp/world_sync.h"
#include "output.h"
#include "point.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "uilist.h"
#include "ui_manager.h"
#include "uistate.h"
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
    // A step was sent and the host hasn't answered yet.
    bool step_pending = false;
    std::chrono::steady_clock::time_point step_sent;
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
    // The host's answers for the crafting screen.
    remote_crafting::client_cache crafting;
    // Open the crafting screen on the next round of the main loop.
    bool show_crafting = false;
    // An action that needs the copy of the character (the host's own
    // screens on the client's avatar); run when the character comes.
    std::optional<action_id> character_action;
    bool character_loaded = false;
    // The host has no character for this player: they make one.
    bool create_character = false;
    // Questions of the host's game, answered in the main loop.
    std::vector<std::string> prompts;
    // Map updates that came before the game data was loaded.
    std::vector<std::string> pending_submaps;
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
            if( type != "status" ) {
                state.step_pending = false;
            }
            state.status = format_status( msg.get_object( "status" ) );
            if( type == "your_turn" ) {
                state.my_turn = true;
            }
        } else if( type == "welcome" ) {
            state.character = msg.get_string( "npc", "" );
            state.instant = msg.get_bool( "instant", false );
            state.create_character = msg.get_bool( "create", false );
            // Again after the second player made a character: the data is loaded.
            if( msg.has_array( "mods" ) && state.mods.empty() ) {
                for( const std::string mod : msg.get_array( "mods" ) ) {
                    state.mods.push_back( mod );
                }
                // Tiles and the crafting screen need the host's data.
                state.data_requested = true;
            }
            if( msg.get_int( "version", 0 ) != protocol::version ) {
                state.add_log( _( "Warning: the host runs a different version of the game." ), c_yellow );
            }
            state.add_log( state.character.empty() ?
                           _( "Connected.  Make your character." ) :
                           string_format( _( "Connected.  You play %s." ), state.character ), c_light_green );
        } else if( type == "log" ) {
            for( const std::string text : msg.get_array( "lines" ) ) {
                state.add_log( text );
                // For the game's own message panel.
                Messages::add_msg( text );
            }
        } else if( type == "rejected" ) {
            state.step_pending = false;
            state.add_log( string_format( _( "Can't do that: %s" ), msg.get_string( "reason", "" ) ),
                           c_light_red );
        } else if( type == "chat" ) {
            player_talk::show( msg );
            state.add_log( string_format( "%s: %s", msg.get_string( "from", "" ), msg.get_string( "text", "" ) ),
                           c_light_cyan );
        } else if( type == "personal" ) {
            remote_log::read( msg );
            state.add_log( msg.get_string( "text", "" ), c_white );
        } else if( type == "prompt" ) {
            state.prompts.push_back( line );
        } else if( type == "submaps" || type == "creatures" || type == "world" || type == "overmap" ||
                   type == "position" ) {
            if( !state.data_loaded ) {
                state.pending_submaps.push_back( line );
            } else if( type == "submaps" ) {
                world_sync::read( msg );
            } else if( type == "creatures" ) {
                world_sync::read_creatures( msg );
            } else if( type == "position" ) {
                world_sync::read_position( msg );
            } else if( type == "overmap" ) {
                world_sync::read_overmap( msg );
            } else {
                world_sync::read_world( msg );
            }
        } else if( type == "character" ) {
            if( !state.data_loaded ) {
                state.pending_submaps.push_back( line );
                return;
            }
            state.character_loaded = remote_actions::load_character( msg.get_string( "data" ) );
            if( state.character_loaded ) {
                world_sync::follow_avatar();
            } else {
                state.add_log( _( "Can't read the character from the host." ), c_red );
                state.character_action.reset();
            }
        } else if( remote_crafting::read_message( state.crafting, type, msg ) ) {
            // Taken by the crafting screen.
        } else if( type == "sidebar" ) {
            state.sidebar.clear();
            for( const std::string line : msg.get_array( "lines" ) ) {
                state.sidebar.push_back( line );
            }
        } else if( type == "sfx" ) {
            sound_relay::play( msg );
        } else if( type == "error" ) {
            state.add_log( msg.get_string( "message", "" ), c_red );
        }
    } catch( const JsonError &err ) {
        state.add_log( string_format( _( "Bad message from the host: %s" ), err.what() ), c_red );
    }
}

// The game's character creation (as for a new game) with the host's data;
// the host makes the character next to its own. False if the player gave up.
bool make_character()
{
    while( true ) {
        avatar &u = get_avatar();
        u = avatar();
        if( u.create( character_type::CUSTOM ) ) {
            std::ostringstream data;
            JsonOut data_json( data );
            u.serialize( data_json );
            std::ostringstream os;
            JsonOut json( os );
            json.start_object();
            json.member( "cmd", "new_character" );
            json.member( "data", data.str() );
            json.end_object();
            net::client_send_line( os.str() );
            return true;
        }
        if( query_yn( _( "Leave the game?" ) ) ) {
            return false;
        }
    }
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
    if( cmd != "status" && cmd != "inventory" && cmd != "character" ) {
        state.my_turn = false;
    }
    if( cmd == "move" ) {
        state.step_pending = true;
        state.step_sent = std::chrono::steady_clock::now();
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
        case ACTION_CRAFT:
            state.show_crafting = true;
            break;
        default:
            if( world_actions::run( act ) ) {
                break;
            }
            if( remote_actions::uses_character( act ) ) {
                state.character_action = act;
                state.character_loaded = false;
                send_command( state, "character" );
                break;
            }
            state.add_log( string_format( _( "%s: not available to the second player yet." ),
                                          ctxt.get_action_name( action ) ), c_dark_gray );
            break;
        case ACTION_MESSAGES:
            show_messages( state );
            break;
        case ACTION_MAIN_MENU:
        case ACTION_ACTIONMENU: {
            if( !state.data_loaded || !state.character_loaded ) {
                return act == ACTION_MAIN_MENU ? !query_yn( _( "Leave the game?" ) ) : true;
            }
            // The host's menus (game::handle_action()), on the copy.
            const action_id chosen = act == ACTION_MAIN_MENU ? handle_main_menu() :
                                     handle_action_menu( get_map() );
            if( chosen == ACTION_NULL || chosen == act ) {
                return true;
            }
            return handle_action( state, action_ident( chosen ), ctxt );
        }
        case ACTION_SAVE:
        case ACTION_QUICKSAVE:
            // The host saves the world; the second player can only leave.
            return !query_yn( _( "Leave the game?" ) );
        case ACTION_KEYBINDINGS: {
            input_context keys = get_default_mode_input_context();
            keys.display_menu();
            break;
        }
        // This client's own settings and view: the host's code as it is.
        case ACTION_OPTIONS:
        case ACTION_AUTOPICKUP:
        case ACTION_AUTONOTES:
        case ACTION_SAFEMODE:
        case ACTION_DISTRACTION_MANAGER:
        case ACTION_COLOR:
        case ACTION_WORLD_MODS:
        case ACTION_TOGGLE_FULLSCREEN:
        case ACTION_TOGGLE_PIXEL_MINIMAP:
        case ACTION_TOGGLE_PANEL_ADM:
        case ACTION_RELOAD_TILESET:
        case ACTION_TOGGLE_AUTO_FEATURES:
        case ACTION_TOGGLE_AUTO_PULP_BUTCHER:
        case ACTION_TOGGLE_AUTO_MINING:
        case ACTION_TOGGLE_AUTO_FORAGING:
        case ACTION_TOGGLE_AUTO_PICKUP:
        case ACTION_TOGGLE_HOUR_TIMER:
        case ACTION_TOGGLE_PREVENT_OCCLUSION:
        case ACTION_ZOOM_IN:
        case ACTION_ZOOM_OUT:
            if( state.data_loaded ) {
                g->do_action_for_mirror( act );
            } else if( act == ACTION_OPTIONS ) {
                get_options().show( false );
            }
            break;
        case ACTION_NULL:
        case ACTION_TIMEOUT:
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
        // What game::setup() does after loading the data, for the game's own
        // screen: a fresh map (sized for traps...), the sidebar's panels,
        // the calendar's settings and an empty log.
        get_map() = map();
        panel_manager::get_manager().init();
        calendar::set_eternal_season( ::get_option<bool>( "ETERNAL_SEASON" ) );
        calendar::set_season_length( ::get_option<int>( "SEASON_LENGTH" ) );
        calendar::set_location( ::get_option<float>( "LATITUDE" ), ::get_option<float>( "LONGITUDE" ) );
        get_weather().weather_id = WEATHER_CLEAR;
        Messages::clear_messages();
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
    // The game's own main screen (map, sidebar, messages), drawn from the
    // copies of the map, the character and the creatures, once they are here.
    shared_ptr_fast<ui_adaptor> game_screen;
    ui.on_redraw( [&]( const ui_adaptor & ) {
        if( game_screen ) {
            return;
        }
        draw_sidebar( w_side, state );
        draw_map( w_map, state );
    } );

    net::handlers h;
    h.on_line = [&]( const std::string & line ) {
        handle_message( state, line );
        ui.invalidate_ui();
        // Every step its own frame, as the host sees it, even when several
        // arrive at once.
        if( game_screen && line.find( R"("type":"state")" ) != std::string::npos ) {
            ui_manager::redraw();
            refresh_display();
        }
    };
    h.on_disconnect = [&]() {
        state.lost = true;
        state.my_turn = false;
        state.add_log( _( "The connection to the host is lost." ), c_red );
        ui.invalidate_ui();
    };

    // The host's own screens open on the copy of the character from now on.
    remote_actions::set_client_active( true );
    // The host's own keys.
    input_context ctxt = get_default_mode_input_context();
    ctxt.set_timeout( 100 );
    while( true ) {
        net::client_poll( h );
        if( state.data_requested ) {
            load_host_data( state );
            ui.mark_resize();
            const std::vector<std::string> pending = std::move( state.pending_submaps );
            state.pending_submaps.clear();
            for( const std::string &line : pending ) {
                handle_message( state, line );
            }
        }
        if( !game_screen && state.data_loaded && state.character_loaded && !state.lost ) {
            game_screen = g->create_or_get_main_ui_adaptor();
            // The host can stop sending the simple screen's data.
            net::client_send_line( R"({"cmd":"screen","action":"native"})" );
        }
        while( !state.prompts.empty() && !state.lost ) {
            const std::string line = state.prompts.front();
            state.prompts.erase( state.prompts.begin() );
            try {
                const JsonValue value = json_loader::from_string( line );
                remote_prompt::answer( value.get_object() );
            } catch( const JsonError &err ) {
                state.add_log( string_format( _( "Bad message from the host: %s" ), err.what() ), c_red );
            }
            ui.invalidate_ui();
        }
        if( state.create_character && state.data_loaded && !state.lost ) {
            state.create_character = false;
            if( !make_character() ) {
                break;
            }
            ui.invalidate_ui();
        }
        if( state.character_action && state.character_loaded && !state.lost ) {
            const action_id act = *state.character_action;
            state.character_action.reset();
            const size_t messages_before = Messages::size();
            remote_actions::run( act );
            // What the game's screens said here ("There is nothing to pick up.").
            const size_t count = Messages::size() - std::min( Messages::size(), messages_before );
            for( const std::pair<std::string, std::string> &m : Messages::recent_messages( count ) ) {
                state.add_log( m.second );
            }
            ui.invalidate_ui();
        }
        if( state.show_crafting && !state.lost ) {
            state.show_crafting = false;
            remote_crafting::show_screen( state.crafting, [&]() {
                net::client_poll( h );
                return !state.lost;
            } );
            ui.invalidate_ui();
        }
        if( state.character_loaded && !state.lost && !state.step_pending ) {
            const size_t messages_before = Messages::size();
            remote_actions::reopen_menu();
            const size_t count = Messages::size() - std::min( Messages::size(), messages_before );
            for( const std::pair<std::string, std::string> &m : Messages::recent_messages( count ) ) {
                state.add_log( m.second );
            }
        }
        // As on the host, a held key moves one step per frame: the next key
        // is read only when the host has answered the step (or is slow).
        if( state.step_pending && !state.lost &&
            std::chrono::steady_clock::now() - state.step_sent < std::chrono::seconds( 1 ) ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            continue;
        }
        state.step_pending = false;
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
        // Another action instead of waiting for the menu to come back.
        uistate.open_menu.reset();
        if( !handle_action( state, action, ctxt ) ) {
            break;
        }
        ui.invalidate_ui();
    }
    game_screen.reset();
    net::disconnect();
    remote_actions::set_client_active( false );
    // The avatar held a copy of the remote character.
    get_avatar() = avatar();
    // And the map buffer, copies of the host's submaps.
    MAPBUFFER.clear();
    world_sync::forget_host();
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
