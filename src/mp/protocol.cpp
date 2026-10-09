#include "mp/protocol.h"

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"
#include "json.h"
#include "json_loader.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/player_talk.h"
#include "mp/rc_npc.h"
#include "mp/remote_actions.h"
#include "mp/remote_crafting.h"
#include "mp/remote_log.h"
#include "mp/remote_prompt.h"
#include "mp/remote_inventory.h"
#include "mp/remote_sidebar.h"
#include "mp/remote_vehicle.h"
#include "mp/view.h"
#include "mp/world_actions.h"
#include "mp/world_sync.h"
#include "npc.h"
#include "scenario.h"
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
// A state was due but held back by the limit.
bool state_pending = false;
// The client shows the game's own screen from the copies: the simple view
// and sidebar are not needed any more (they are most of a step's traffic).
bool client_native_screen = false;

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
        if( !is_host_only( recent[i].second ) && remote_log::is_shared( recent[i].second ) ) {
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

// Shared time: commands that came while the character was busy, in order.
std::deque<std::string> deferred;

// What takes the character's time; other commands are answered at once.
bool takes_time( const std::string &cmd_name )
{
    static const std::set<std::string> commands = {
        "move", "attack", "wait", "pickup", "craft", "tile_action", "combat", "activity",
        "construct", "power", "talk", "item_action", "item"
    };
    return commands.count( cmd_name ) > 0;
}

// After a command was taken: in instant time everything it started is done
// now, in shared time the queued step runs with the moves there are.
void after_command( npc &guy )
{
    if( instant_mode() ) {
        run_instantly( guy );
    } else if( shared_time() && has_commands( guy ) && guy.get_moves() > 0 ) {
        remote_move( guy );
    }
}

void handle_line_from( const std::string &line, bool from_queue );

void handle_line( const std::string &line )
{
    handle_line_from( line, false );
}

void handle_line_from( const std::string &line, const bool from_queue )
{
    const reading_network reading;
    std::string cmd_name;
    std::string dir_name;
    // For "item".
    int revision = 0;
    int index = -1;
    std::string action;
    // For "craft".
    std::string recipe;
    int batch = 1;
    // For "say".
    std::string say_text;
    // For "new_character".
    std::string character_data;
    // For "tile_action".
    std::optional<tripoint_rel_ms> tile_dir;
    try {
        const JsonValue value = json_loader::from_string( line );
        const JsonObject obj = value.get_object();
        obj.allow_omitted_members();
        cmd_name = obj.get_string( "cmd" );
        if( obj.has_string( "dir" ) ) {
            dir_name = obj.get_string( "dir" );
        }
        revision = obj.get_int( "revision", 0 );
        index = obj.get_int( "index", -1 );
        action = obj.get_string( "action", "" );
        say_text = obj.get_string( "text", "" );
        recipe = obj.get_string( "recipe", "" );
        if( cmd_name == "new_character" ) {
            character_data = obj.get_string( "data", "" );
        }
        if( obj.has_array( "offset" ) ) {
            JsonArray d = obj.get_array( "offset" );
            const int x = d.next_int();
            const int y = d.next_int();
            const int z = d.next_int();
            tile_dir = tripoint_rel_ms( x, y, z );
        }
        batch = obj.get_int( "batch", 1 );
    } catch( const JsonError &err ) {
        send_error( "bad message: " + std::string( err.what() ) );
        return;
    }

    if( cmd_name == "screen" ) {
        client_native_screen = action == "native";
        return;
    }
    if( cmd_name == "new_character" ) {
        std::string error;
        if( const npc *made = partner_from_client( character_data, error ) ) {
            send_welcome();
            send_state( *made );
        } else {
            send_error( error );
        }
        return;
    }
    npc *guy = network_npc();
    if( guy == nullptr ) {
        send_error( "the host has no remote-controlled NPC nearby" );
        return;
    }
    if( shared_time() && takes_time( cmd_name ) &&
        ( busy( *guy ) || ( !from_queue && !deferred.empty() ) ) ) {
        // Done when the character has time for it (poll()).
        deferred.push_back( line );
        return;
    }
    // Whatever the game asks while doing this, it asks the second player.
    remote_prompt::asking_client asking;
    if( cmd_name == "status" ) {
        send_status( *guy, "status" );
        return;
    }
    if( cmd_name == "say" ) {
        if( !say_text.empty() ) {
            player_talk::say_from_client( *guy, say_text );
        }
        return;
    }
    if( cmd_name == "inventory" ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "inventory" );
            inventory::write( json, *guy );
        } ) );
        return;
    }
    // The crafting screen's questions; answered with a message of the same name.
    using craft_writer = void( * )( JsonOut &, npc &, const JsonObject & );
    static const std::map<std::string, craft_writer> craft_questions = {
        { "recipe_states", remote_crafting::write_states },
        { "recipe_info", remote_crafting::write_info },
        { "recipe_filter", remote_crafting::write_filter },
    };
    if( cmd_name == "character" ) {
        // The whole character, for the client's copy of the host's screens.
        const std::string data = world_sync::character_if_changed( *guy, true );
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "character" );
            json.member( "data", data );
        } ) );
        return;
    }
    if( cmd_name == "recipes" ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "recipes" );
            remote_crafting::write_recipes( json, *guy );
        } ) );
        return;
    }
    if( const auto question = craft_questions.find( cmd_name ); question != craft_questions.end() ) {
        try {
            const JsonValue value = json_loader::from_string( line );
            const JsonObject obj = value.get_object();
            obj.allow_omitted_members();
            const std::string answer = to_line( [&]( JsonOut & json ) {
                json.member( "type", cmd_name );
                question->second( json, *guy, obj );
            } );
            net::send_line( answer );
        } catch( const JsonError &err ) {
            send_error( "bad message: " + std::string( err.what() ) );
        }
        return;
    }
    if( cmd_name == "craft" ) {
        if( !net::running() ) {
            send_error( "crafting works only while the server runs" );
            return;
        }
        const std::string why_not = remote_crafting::start( *guy, recipe, batch );
        if( why_not.empty() ) {
            after_command( *guy );
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
    if( cmd_name == "tile_action" ) {
        const std::string why_not = tile_dir ? world_actions::act( *guy, action, *tile_dir ) :
                                    std::string( "no dir" );
        if( why_not.empty() ) {
            after_command( *guy );
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
    if( cmd_name == "combat" || cmd_name == "activity" || cmd_name == "construct" ||
        cmd_name == "move_mode" || cmd_name == "setting" || cmd_name == "power" ||
        cmd_name == "talk" || cmd_name == "vehicle_edit" ) {
        std::string why_not;
        try {
            const JsonValue value = json_loader::from_string( line );
            const JsonObject obj = value.get_object();
            obj.allow_omitted_members();
            why_not = cmd_name == "combat" ? remote_actions::combat( *guy, obj ) :
                      cmd_name == "activity" ? remote_actions::activity( *guy, obj ) :
                      cmd_name == "construct" ? remote_actions::construct( *guy, obj ) :
                      cmd_name == "setting" ? remote_actions::setting( *guy, obj ) :
                      cmd_name == "power" ? remote_actions::power( *guy, obj ) :
                      cmd_name == "talk" ? remote_actions::talk( *guy, obj ) :
                      cmd_name == "vehicle_edit" ? remote_vehicle::edit( *guy, obj ) :
                      remote_actions::move_mode( *guy, obj );
        } catch( const JsonError &err ) {
            why_not = "bad message: " + std::string( err.what() );
        }
        if( why_not.empty() ) {
            after_command( *guy );
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
    if( cmd_name == "item_action" ) {
        std::string why_not;
        try {
            const JsonValue value = json_loader::from_string( line );
            const JsonObject obj = value.get_object();
            obj.allow_omitted_members();
            why_not = remote_actions::item_action( *guy, obj );
        } catch( const JsonError &err ) {
            why_not = "bad message: " + std::string( err.what() );
        }
        if( why_not.empty() ) {
            after_command( *guy );
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
    if( cmd_name == "item" ) {
        if( !net::running() ) {
            send_error( "items can be used only while the server runs" );
            return;
        }
        const std::string why_not = inventory::act( *guy, revision, index, action );
        if( why_not.empty() ) {
            after_command( *guy );
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
    if( instant_mode() || shared_time() ) {
        after_command( *guy );
        send_state( *guy );
    }
}

} // namespace

static int network_reads = 0;

bool ignore_item_uids()
{
    return network_reads > 0 || remote_actions::client_active();
}

reading_network::reading_network()
{
    network_reads++;
}

reading_network::~reading_network()
{
    network_reads--;
}

void send_welcome()
{
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "welcome" );
        json.member( "version", version );
        json.member( "instant", instant_mode() );
        json.member( "shared_time", shared_time() );
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
        } else {
            // The second player makes one (new_character).
            json.member( "create", true );
        }
    } ) );
}

void send_view( const npc &guy )
{
    if( client_native_screen ) {
        return;
    }
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "view" );
        view::write( json, guy );
    } ) );
}

void send_state( const npc &guy )
{
    send_new_messages();
    if( !client_native_screen ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "sidebar" );
            remote_sidebar::write( json, guy );
        } ) );
    }
    send_view( guy );
    // The map around the character, for the game's own screens on the client.
    bool submaps_changed = false;
    const std::string submaps = to_line( [&]( JsonOut & json ) {
        json.member( "type", "submaps" );
        submaps_changed = world_sync::write_changed( json, guy );
    } );
    if( submaps_changed ) {
        net::send_line( submaps );
    }
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "world" );
        world_sync::write_world( json );
    } ) );
    bool moved = false;
    const std::string character = world_sync::character_if_changed( guy, false, &moved );
    if( moved ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "position" );
            json.member( "at" );
            json.start_array();
            json.write( guy.pos_abs().x() );
            json.write( guy.pos_abs().y() );
            json.write( guy.pos_abs().z() );
            json.end_array();
        } ) );
    }
    if( !character.empty() ) {
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "character" );
            json.member( "data", character );
        } ) );
    }
    bool overmap_changed = false;
    const std::string overmap = to_line( [&]( JsonOut & json ) {
        json.member( "type", "overmap" );
        overmap_changed = world_sync::write_overmap( json, guy );
    } );
    if( overmap_changed ) {
        net::send_line( overmap );
    }
    bool creatures_changed = false;
    const std::string creatures = to_line( [&]( JsonOut & json ) {
        json.member( "type", "creatures" );
        creatures_changed = world_sync::write_creatures( json, guy );
    } );
    if( creatures_changed ) {
        net::send_line( creatures );
    }
    send_status( guy, "state" );
    last_state_sent = std::chrono::steady_clock::now();
    state_pending = false;
}

void send_state_if_due()
{
    if( !net::has_client() ) {
        return;
    }
    // At most a few times a second, so that a sleeping host doesn't flood
    // the network; a state skipped here goes with send_pending_state().
    if( std::chrono::steady_clock::now() - last_state_sent < std::chrono::milliseconds( 250 ) ) {
        state_pending = true;
        return;
    }
    if( const npc *guy = network_npc() ) {
        send_state( *guy );
    }
}

void mark_changed()
{
    state_pending = true;
}

void send_pending_state()
{
    if( state_pending ) {
        send_state_if_due();
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

bool has_deferred()
{
    return !deferred.empty();
}

void poll()
{
    // Shared time: what waited for the character's time.
    while( shared_time() && !deferred.empty() ) {
        const npc *guy = network_npc();
        if( guy == nullptr || busy( *guy ) ) {
            break;
        }
        const std::string line = deferred.front();
        deferred.pop_front();
        handle_line_from( line, true );
    }
    // Commands that came while the host waited for an answer to a question.
    for( const std::string &line : remote_prompt::take_deferred() ) {
        handle_line( line );
    }
    net::handlers h;
    h.on_line = handle_line;
    h.on_connect = []() {
        add_msg( m_info, _( "The second player has connected." ) );
        skip_old_messages();
        world_sync::reset();
        deferred.clear();
        client_native_screen = false;
        send_welcome();
        // Something to look at right away, not after the first action.
        if( const npc *guy = network_npc() ) {
            send_state( *guy );
        }
    };
    h.on_disconnect = []() {
        deferred.clear();
        add_msg( m_warning, _( "The second player has disconnected." ) );
    };
    net::poll( h );
}

} // namespace mp::protocol
