#include "mp/protocol.h"

#include <chrono>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"
#include "json.h"
#include "json_loader.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/rc_npc.h"
#include "mp/remote_inventory.h"
#include "mp/remote_sidebar.h"
#include "mp/view.h"
#include "npc.h"
#include "translations.h"
#include "type_id.h"
#include "worldfactory.h"

namespace mp::protocol
{

namespace
{

template<typename Writer>
std::string to_line( const Writer &write )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    write( json );
    json.end_object();
    return os.str();
}

// When the client last got the view and the status.
std::chrono::steady_clock::time_point last_state_sent;

// The newest message of the host's log already sent to the client.
std::optional<std::pair<std::string, std::string>> last_sent_message;

// Messages that only make sense on the host's screen.
bool is_host_only( const std::string &text )
{
    static const std::string waiting = _( "Waiting for" );
    return text.compare( 0, waiting.size(), waiting ) == 0;
}

// Forgets the log so far: a new client starts with an empty log.
void skip_old_messages()
{
    const std::vector<std::pair<std::string, std::string>> recent = Messages::recent_messages( 1 );
    if( recent.empty() ) {
        last_sent_message.reset();
    } else {
        last_sent_message = recent.back();
    }
}

void send_new_messages()
{
    const std::vector<std::pair<std::string, std::string>> recent = Messages::recent_messages( 30 );
    size_t first_new = 0;
    if( last_sent_message ) {
        for( size_t i = recent.size(); i > 0; i-- ) {
            if( recent[i - 1] == *last_sent_message ) {
                first_new = i;
                break;
            }
        }
    }
    std::vector<std::string> lines;
    for( size_t i = first_new; i < recent.size(); i++ ) {
        if( !is_host_only( recent[i].second ) ) {
            lines.push_back( recent[i].second );
        }
    }
    if( !recent.empty() ) {
        last_sent_message = recent.back();
    }
    if( lines.empty() ) {
        return;
    }
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "log" );
        json.member( "lines", lines );
    } ) );
}

void send_error( const std::string &message )
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "error" );
        json.member( "message", message );
    } ) );
}

void write_status( JsonOut &json, const npc &guy )
{
    const tripoint_abs_ms pos = guy.pos_abs();
    json.member( "name", guy.get_name() );
    json.member( "hp", guy.get_hp() );
    json.member( "hp_max", guy.get_hp_max() );
    json.member( "stamina", guy.get_stamina() );
    json.member( "stamina_max", guy.get_stamina_max() );
    json.member( "hunger", guy.get_hunger() );
    json.member( "thirst", guy.get_thirst() );
    json.member( "sleepiness", guy.get_sleepiness() );
    json.member( "pain", guy.get_pain() );
    json.member( "moves", guy.get_moves() );
    json.member( "pos" );
    json.start_array();
    json.write( pos.x() );
    json.write( pos.y() );
    json.write( pos.z() );
    json.end_array();
}

void send_status( const npc &guy, const std::string &type )
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", type );
        json.member( "status" );
        json.start_object();
        write_status( json, guy );
        json.end_object();
    } ) );
}

std::optional<point_rel_ms> parse_dir( const std::string &dir )
{
    static const std::map<std::string, point_rel_ms> dirs = {
        { "n", point_rel_ms::north },
        { "ne", point_rel_ms::north_east },
        { "e", point_rel_ms::east },
        { "se", point_rel_ms::south_east },
        { "s", point_rel_ms::south },
        { "sw", point_rel_ms::south_west },
        { "w", point_rel_ms::west },
        { "nw", point_rel_ms::north_west },
    };
    const auto it = dirs.find( dir );
    if( it == dirs.end() ) {
        return std::nullopt;
    }
    return it->second;
}

void handle_line( const std::string &line )
{
    std::string cmd_name;
    std::string dir_name;
    // For "item".
    int revision = 0;
    int index = -1;
    std::string action;
    try {
        const JsonValue value = json_loader::from_string( line );
        const JsonObject obj = value.get_object();
        obj.allow_omitted_members();
        cmd_name = obj.get_string( "cmd" );
        dir_name = obj.get_string( "dir", "" );
        revision = obj.get_int( "revision", 0 );
        index = obj.get_int( "index", -1 );
        action = obj.get_string( "action", "" );
    } catch( const JsonError &err ) {
        send_error( "bad message: " + std::string( err.what() ) );
        return;
    }

    npc *guy = network_npc();
    if( guy == nullptr ) {
        send_error( "the host has no remote-controlled NPC nearby" );
        return;
    }
    if( cmd_name == "status" ) {
        send_status( *guy, "status" );
        return;
    }
    if( cmd_name == "inventory" ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "inventory" );
            inventory::write( json, *guy );
        } ) );
        return;
    }
    if( cmd_name == "item" ) {
        if( !instant_mode() ) {
            send_error( "items can be used only while the server runs" );
            return;
        }
        const std::string why_not = inventory::act( *guy, revision, index, action );
        if( why_not.empty() ) {
            run_instantly( *guy );
            net::send_line( to_line( [&]( JsonOut & json ) {
                json.member( "type", "ok" );
                json.member( "cmd", cmd_name );
            } ) );
        } else {
            send_rejected( why_not );
        }
        send_state( *guy );
        return;
    }

    command cmd;
    if( cmd_name == "move" || cmd_name == "attack" ) {
        const std::optional<point_rel_ms> dir = parse_dir( dir_name );
        if( !dir ) {
            send_error( "bad dir \"" + dir_name + "\", expected n, ne, e, se, s, sw, w or nw" );
            return;
        }
        cmd.type = cmd_name == "move" ? command_type::move : command_type::attack;
        cmd.dir = *dir;
    } else if( cmd_name == "wait" ) {
        cmd.type = command_type::wait;
    } else if( cmd_name == "pickup" ) {
        cmd.type = command_type::pickup_all;
    } else {
        send_error( "unknown cmd \"" + cmd_name + "\"" );
        return;
    }
    push_command( *guy, cmd );
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "ok" );
        json.member( "cmd", cmd_name );
    } ) );
    if( instant_mode() ) {
        run_instantly( *guy );
        send_state( *guy );
    }
}

} // namespace

void send_welcome()
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "welcome" );
        json.member( "version", version );
        json.member( "instant", instant_mode() );
        // The client loads the same data to draw tiles.
        json.member( "mods" );
        json.start_array();
        if( world_generator->active_world != nullptr ) {
            for( const mod_id &mod : world_generator->active_world->active_mod_order ) {
                json.write( mod.str() );
            }
        }
        json.end_array();
        const npc *guy = network_npc();
        if( guy != nullptr ) {
            json.member( "npc", guy->get_name() );
        }
    } ) );
}

void send_view( const npc &guy )
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "view" );
        view::write( json, guy );
    } ) );
}

void send_state( const npc &guy )
{
    send_new_messages();
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "sidebar" );
        remote_sidebar::write( json, guy );
    } ) );
    send_view( guy );
    send_status( guy, "state" );
    last_state_sent = std::chrono::steady_clock::now();
}

void send_state_if_due()
{
    if( !net::has_client() ) {
        return;
    }
    // At most a few times a second, so that a sleeping host doesn't flood
    // the network.
    if( std::chrono::steady_clock::now() - last_state_sent < std::chrono::milliseconds( 250 ) ) {
        return;
    }
    if( const npc *guy = network_npc() ) {
        send_state( *guy );
    }
}

void send_your_turn( const npc &guy )
{
    send_new_messages();
    send_view( guy );
    send_status( guy, "your_turn" );
}

void send_rejected( const std::string &reason )
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "rejected" );
        json.member( "reason", reason );
    } ) );
}

void poll()
{
    net::handlers h;
    h.on_line = handle_line;
    h.on_connect = []() {
        add_msg( m_info, _( "The second player has connected." ) );
        skip_old_messages();
        send_welcome();
        // Something to look at right away, not after the first action.
        if( const npc *guy = network_npc() ) {
            send_state( *guy );
        }
    };
    h.on_disconnect = []() {
        add_msg( m_warning, _( "The second player has disconnected." ) );
    };
    net::poll( h );
}

} // namespace mp::protocol
