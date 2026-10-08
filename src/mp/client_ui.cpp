#include "mp/client_ui.h"

#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "color.h"
#include "cursesdef.h"
#include "input.h"
#include "input_context.h"
#include "json.h"
#include "json_loader.h"
#include "mp/net.h"
#include "mp/protocol.h"
#include "mp/view.h"
#include "output.h"
#include "point.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "ui_manager.h"

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
    bool attack_pending = false;
    bool lost = false;
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
        } else if( type == "your_turn" || type == "status" ) {
            state.status = format_status( msg.get_object( "status" ) );
            if( type == "your_turn" ) {
                state.my_turn = true;
            }
        } else if( type == "welcome" ) {
            state.character = msg.get_string( "npc", "" );
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
        } else if( type == "error" ) {
            state.add_log( msg.get_string( "message", "" ), c_red );
        }
    } catch( const JsonError &err ) {
        state.add_log( string_format( _( "Bad message from the host: %s" ), err.what() ), c_red );
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
    } else if( ch == 'q' || ch == KEY_ESCAPE ) {
        return !query_yn( _( "Leave the game?" ) );
    }
    return true;
}

void draw( const catacurses::window &w, const client_state &state )
{
    werase( w );
    const int width = getmaxx( w );
    const int height = getmaxy( w );
    constexpr int log_height = 8;
    const int map_top = 2;
    const int map_height = std::max( 1, height - map_top - log_height - 1 );

    // Top: who we are and whose turn it is.
    const std::string turn = state.lost ? _( "Disconnected" ) :
                             state.my_turn ? _( "YOUR TURN" ) : _( "Waiting for the host…" );
    mvwprintz( w, point( 0, 0 ), state.my_turn ? c_light_green : c_yellow, turn );
    mvwprintz( w, point( utf8_width( turn ) + 2, 0 ), c_white, state.status );

    // Middle: the map, the character in the middle.
    const view::grid &grid = state.grid;
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

    // Bottom: the log and the keys.
    const int log_top = height - log_height - 1;
    const int shown = std::min<int>( log_height, state.log.size() );
    for( int i = 0; i < shown; i++ ) {
        const auto &entry = state.log[state.log.size() - shown + i];
        trim_and_print( w, point( 0, log_top + i ), width, entry.second, entry.first );
    }
    trim_and_print( w, point( 0, height - 1 ), width, c_dark_gray,
                    _( "hjklyubn/arrows/numpad move  a+dir attack  . wait  g pick up  s status  q leave" ) );
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
    ui_adaptor ui;
    ui.on_screen_resize( [&]( ui_adaptor & ui ) {
        w = catacurses::newwin( TERMY, TERMX, point::zero );
        ui.position_from_window( w );
    } );
    ui.mark_resize();
    ui.on_redraw( [&]( const ui_adaptor & ) {
        draw( w, state );
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
}

} // namespace mp::client_ui
