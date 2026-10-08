#include "mp/remote_prompt.h"

#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

#include "action.h"
#include "input_context.h"
#include "json.h"
#include "json_loader.h"
#include "mp/net.h"
#include "output.h"
#include "popup.h"
#include "string_input_popup.h"
#include "uilist.h"

namespace mp::remote_prompt
{

namespace
{

int depth = 0;
int next_id = 1;
std::vector<std::string> deferred;

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

// Sends the question and waits for the answer; the answer's object, or
// std::nullopt if the client went away or took too long.
template<typename Writer>
std::optional<std::string> ask( const std::string &kind, const Writer &write )
{
    if( !net::has_client() ) {
        return std::nullopt;
    }
    const int id = next_id++;
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "prompt" );
        json.member( "id", id );
        json.member( "kind", kind );
        write( json );
    } ) );
    std::optional<std::string> answer;
    bool gone = false;
    net::handlers h;
    h.on_line = [&]( const std::string & line ) {
        try {
            const JsonValue value = json_loader::from_string( line );
            const JsonObject obj = value.get_object();
            obj.allow_omitted_members();
            if( obj.get_string( "cmd", "" ) == "prompt_answer" && obj.get_int( "id", 0 ) == id ) {
                answer = line;
                return;
            }
        } catch( const JsonError & ) {
            // Not an answer; the protocol reports it later.
        }
        deferred.push_back( line );
    };
    h.on_disconnect = [&]() {
        gone = true;
    };
    // Long enough to read a menu; the host's game stands still meanwhile.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::minutes( 5 );
    while( !answer && !gone && net::has_client() && std::chrono::steady_clock::now() < give_up ) {
        net::poll( h );
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    return answer;
}

template<typename Reader>
void read_answer( const std::optional<std::string> &line, const Reader &read )
{
    if( !line ) {
        return;
    }
    try {
        const JsonValue value = json_loader::from_string( *line );
        const JsonObject obj = value.get_object();
        obj.allow_omitted_members();
        read( obj );
    } catch( const JsonError & ) {
        // A broken answer counts as cancel.
    }
}

void send_answer( const int id, const std::function<void( JsonOut & )> &write )
{
    net::client_send_line( to_line( [&]( JsonOut & json ) {
        json.member( "cmd", "prompt_answer" );
        json.member( "id", id );
        write( json );
    } ) );
}

} // namespace

asking_client::asking_client()
{
    depth++;
}

asking_client::~asking_client()
{
    depth--;
}

bool active()
{
    return depth > 0 && net::has_client();
}

std::vector<std::string> take_deferred()
{
    return std::exchange( deferred, {} );
}

std::optional<int> ask_uilist( const uilist &menu )
{
    if( !active() ) {
        return std::nullopt;
    }
    const std::optional<std::string> line = ask( "uilist", [&]( JsonOut & json ) {
        json.member( "title", menu.title );
        json.member( "text", menu.text );
        json.member( "selected", menu.selected );
        json.member( "desc_enabled", menu.desc_enabled );
        json.member( "entries" );
        json.start_array();
        for( const uilist_entry &e : menu.entries ) {
            json.start_object();
            json.member( "retval", e.retval );
            json.member( "enabled", e.enabled );
            json.member( "txt", e.txt );
            json.member( "desc", e.desc );
            json.member( "ctxt", e.ctxt );
            json.member( "color", string_from_color( e.text_color ) );
            json.end_object();
        }
        json.end_array();
    } );
    int ret = UILIST_CANCEL;
    read_answer( line, [&]( const JsonObject & obj ) {
        ret = obj.get_int( "ret", UILIST_CANCEL );
    } );
    return ret;
}

std::optional<std::string> ask_popup( const std::string &text,
                                      const std::vector<std::string> &actions, const std::string &category,
                                      const bool allow_cancel, const bool allow_anykey )
{
    if( !active() ) {
        return std::nullopt;
    }
    const bool just_a_message = actions.empty() && !allow_anykey;
    if( just_a_message ) {
        // Nothing to answer: shown there, the host goes on.
        net::send_line( to_line( [&]( JsonOut & json ) {
            json.member( "type", "prompt" );
            json.member( "id", 0 );
            json.member( "kind", "message" );
            json.member( "text", text );
        } ) );
        return std::string( "QUIT" );
    }
    const std::optional<std::string> line = ask( "popup", [&]( JsonOut & json ) {
        json.member( "text", text );
        json.member( "actions", actions );
        json.member( "category", category );
        json.member( "cancel", allow_cancel );
        json.member( "anykey", allow_anykey );
    } );
    std::string action = "QUIT";
    read_answer( line, [&]( const JsonObject & obj ) {
        action = obj.get_string( "action", "QUIT" );
    } );
    return action;
}

std::optional<std::optional<tripoint_rel_ms>> ask_direction( const std::string &message,
        const bool allow_vertical )
{
    if( !active() ) {
        return std::nullopt;
    }
    const std::optional<std::string> line = ask( "direction", [&]( JsonOut & json ) {
        json.member( "text", message );
        json.member( "vertical", allow_vertical );
    } );
    std::optional<tripoint_rel_ms> dir;
    read_answer( line, [&]( const JsonObject & obj ) {
        if( obj.has_array( "dir" ) ) {
            JsonArray a = obj.get_array( "dir" );
            const int x = a.next_int();
            const int y = a.next_int();
            const int z = a.next_int();
            dir = tripoint_rel_ms( x, y, z );
        }
    } );
    return dir;
}

std::optional<std::optional<std::string>> ask_string( const std::string &title,
        const std::string &description, const std::string &text, const int width,
        const bool only_digits )
{
    if( !active() ) {
        return std::nullopt;
    }
    const std::optional<std::string> line = ask( "string", [&]( JsonOut & json ) {
        json.member( "title", title );
        json.member( "description", description );
        json.member( "text", text );
        json.member( "width", width );
        json.member( "only_digits", only_digits );
    } );
    std::optional<std::string> result;
    read_answer( line, [&]( const JsonObject & obj ) {
        if( obj.has_string( "text" ) ) {
            result = obj.get_string( "text" );
        }
    } );
    return result;
}

void answer( const JsonObject &question )
{
    question.allow_omitted_members();
    const int id = question.get_int( "id", 0 );
    const std::string kind = question.get_string( "kind", "" );
    if( kind == "message" ) {
        popup( question.get_string( "text", "" ) );
    } else if( kind == "uilist" ) {
        uilist menu;
        menu.title = question.get_string( "title", "" );
        menu.text = question.get_string( "text", "" );
        menu.desc_enabled = question.get_bool( "desc_enabled", false );
        for( JsonObject e : question.get_array( "entries" ) ) {
            e.allow_omitted_members();
            menu.addentry_col( e.get_int( "retval" ), e.get_bool( "enabled", true ), MENU_AUTOASSIGN,
                               e.get_string( "txt", "" ), e.get_string( "ctxt", "" ), e.get_string( "desc", "" ) );
            menu.entries.back().text_color = color_from_string( e.get_string( "color", "c_light_gray" ) );
        }
        menu.selected = question.get_int( "selected", 0 );
        menu.query();
        send_answer( id, [&]( JsonOut & json ) {
            json.member( "ret", menu.ret );
        } );
    } else if( kind == "popup" ) {
        query_popup pop;
        const std::string category = question.get_string( "category", "" );
        if( !category.empty() ) {
            pop.context( category );
        }
        pop.message( "%s", question.get_string( "text", "" ) );
        for( const std::string action : question.get_array( "actions" ) ) {
            pop.option( action );
        }
        pop.allow_cancel( question.get_bool( "cancel", false ) );
        pop.allow_anykey( question.get_bool( "anykey", false ) );
        const query_popup::result res = pop.query();
        send_answer( id, [&]( JsonOut & json ) {
            json.member( "action", res.action );
        } );
    } else if( kind == "direction" ) {
        const std::optional<tripoint_rel_ms> dir = choose_direction( question.get_string( "text", "" ),
                question.get_bool( "vertical", false ) );
        send_answer( id, [&]( JsonOut & json ) {
            if( dir ) {
                json.member( "dir" );
                json.start_array();
                json.write( dir->x() );
                json.write( dir->y() );
                json.write( dir->z() );
                json.end_array();
            }
        } );
    } else if( kind == "string" ) {
        string_input_popup pop;
        pop.title( question.get_string( "title", "" ) )
        .description( question.get_string( "description", "" ) )
        .text( question.get_string( "text", "" ) )
        .width( question.get_int( "width", 20 ) )
        .only_digits( question.get_bool( "only_digits", false ) );
        pop.query();
        send_answer( id, [&]( JsonOut & json ) {
            if( !pop.canceled() ) {
                json.member( "text", pop.text() );
            }
        } );
    }
}

} // namespace mp::remote_prompt
