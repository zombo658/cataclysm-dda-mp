#include "mp/remote_prompt.h"

#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>

#include "action.h"
#include "catacharset.h"
#include "creature_tracker.h"
#include "dialogue.h"
#include "dialogue_imgui.h"
#include "talker.h"
#include "avatar.h"
#include "input_context.h"
#include "iuse_software.h"
#include "json.h"
#include "json_loader.h"
#include "game.h"
#include "map.h"
#include "mp/net.h"
#include "mp/player_talk.h"
#include "mp/protocol.h"
#include "mp/rc_npc.h"
#include "mp/remote_trade.h"
#include "mp/remote_vehicle.h"
#include "npc.h"
#include "output.h"
#include "string_formatter.h"
#include "popup.h"
#include "string_input_popup.h"
#include "translations.h"
#include "ui_manager.h"
#include "uilist.h"

namespace mp::remote_prompt
{

namespace
{

int depth = 0;
int next_id = 1;
int conversation = 0;
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
std::optional<std::string> ask( const std::string &kind, const Writer &write,
                                const std::chrono::minutes patience = std::chrono::minutes( 5 ) )
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
    const auto give_up = std::chrono::steady_clock::now() + patience;
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
    const protocol::reading_network reading;
    try {
        const JsonValue value = json_loader::from_string( *line );
        const JsonObject obj = value.get_object();
        obj.allow_omitted_members();
        read( obj );
    } catch( const JsonError & ) {
        // A broken answer counts as cancel.
    }
}

} // namespace

void send_answer( const int id, const std::function<void( JsonOut & )> &write )
{
    net::client_send_line( to_line( [&]( JsonOut & json ) {
        json.member( "cmd", "prompt_answer" );
        json.member( "id", id );
        write( json );
    } ) );
}

host_question::host_question() : saved_depth( depth )
{
    depth = 0;
}

host_question::~host_question()
{
    depth = saved_depth;
}

std::optional<std::string> ask_json( const std::string &kind,
                                     const std::function<void( JsonOut & )> &write )
{
    if( !active() ) {
        return std::nullopt;
    }
    return ask( kind, write );
}

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

void new_conversation()
{
    conversation++;
}

std::optional<int> ask_dialogue( const std::string &npc_name, const tripoint_abs_ms &at,
                                 const std::string &line,
                                 const std::string &speaker, const nc_color &speaker_color,
                                 const std::vector<talk_data> &responses, const std::vector<bool> &selectable )
{
    if( !active() ) {
        return std::nullopt;
    }
    const std::optional<std::string> answer_line = ask( "dialogue", [&]( JsonOut & json ) {
        json.member( "conversation", conversation );
        json.member( "npc", npc_name );
        json.member( "at", at );
        json.member( "line", line );
        json.member( "speaker", speaker );
        json.member( "speaker_color", string_from_color( speaker_color ) );
        json.member( "responses" );
        json.start_array();
        for( size_t i = 0; i < responses.size(); i++ ) {
            json.start_object();
            json.member( "text", responses[i].text );
            json.member( "hotkey", responses[i].hotkey_desc );
            json.member( "color", string_from_color( responses[i].color ) );
            json.member( "selectable", i < selectable.size() ? static_cast<bool>( selectable[i] ) : true );
            json.end_object();
        }
        json.end_array();
    } );
    int ret = -1;
    read_answer( answer_line, [&]( const JsonObject & obj ) {
        ret = obj.get_int( "ret", -1 );
    } );
    return ret;
}

namespace
{

// dialogue::opt_imgui() on the client: the game's dialogue window over the
// copies of the two characters, with the lines of one conversation.
int answer_dialogue( const JsonObject &question )
{
    struct history_line {
        std::string text;
        std::string speaker;
        nc_color color;
    };
    static std::vector<history_line> history;
    static int shown_conversation = -1;
    const int conv = question.get_int( "conversation", 0 );
    if( conv != shown_conversation ) {
        history.clear();
        shown_conversation = conv;
    }
    const std::string npc_name = question.get_string( "npc", "" );
    history.push_back( { question.get_string( "line", "" ), question.get_string( "speaker", "" ),
                         color_from_string( question.get_string( "speaker_color", "c_white" ) ) } );
    std::vector<talk_data> lines;
    std::vector<bool> selectable;
    std::vector<std::string> hotkeys;
    for( JsonObject r : question.get_array( "responses" ) ) {
        r.allow_omitted_members();
        talk_data td;
        td.text = r.get_string( "text", "" );
        td.hotkey_desc = r.get_string( "hotkey", "" );
        td.color = color_from_string( r.get_string( "color", "c_white" ) );
        lines.push_back( td );
        hotkeys.push_back( td.hotkey_desc );
        selectable.push_back( r.get_bool( "selectable", true ) );
    }

    // The window draws its sidebar from the conversation: the copies.
    avatar &you = get_avatar();
    Creature *other = nullptr;
    if( question.has_array( "at" ) ) {
        tripoint_abs_ms at;
        question.read( "at", at );
        other = get_creature_tracker().creature_at( at );
    }
    dialogue d( get_talker_for( you ), get_talker_for( other != nullptr ? *other :
                static_cast<Creature &>( you ) ), {} );
    int chosen = -2;
    {
        dialogue_imgui_impl d_img( &d, false, npc_name.empty(), std::string() );
        for( const history_line &h : history ) {
            if( h.speaker.empty() ) {
                d_img.add_to_history( h.text );
            } else {
                d_img.add_to_history( h.text, h.speaker, h.color );
            }
        }
        d_img.set_responses( lines );
        d_img.sel_response = 0;
        d_img.scroll_to = cataimgui::scroll::end;

        input_context ctxt( "DIALOGUE" );
        ctxt.register_updown();
        ctxt.register_action( "CONFIRM" );
        ctxt.register_action( "HOME" );
        ctxt.register_action( "PAGE_DOWN" );
        ctxt.register_action( "END" );
        ctxt.register_action( "PAGE_UP" );
        ctxt.register_action( "ANY_INPUT" );
        ctxt.register_action( "HELP_KEYBINDINGS" );
        ctxt.register_action( "QUIT" );
        ctxt.set_timeout( 10 );
        while( chosen == -2 ) {
            ui_manager::redraw_invalidated();
            const std::string action = ctxt.handle_input();
            const input_event evt = ctxt.get_raw_input();
            const int count = static_cast<int>( lines.size() );
            if( action == "CONFIRM" || d_img.user_clicked_response_button ) {
                d_img.user_clicked_response_button = false;
                const int i = d_img.sel_response;
                if( i >= 0 && i < count && selectable[i] ) {
                    chosen = i;
                }
            } else if( action == "DOWN" ) {
                d_img.sel_response = std::min( count - 1, d_img.sel_response + 1 );
            } else if( action == "UP" ) {
                d_img.sel_response = std::max( 0, d_img.sel_response - 1 );
            } else if( action == "END" ) {
                d_img.scroll_to = cataimgui::scroll::page_down;
            } else if( action == "HOME" ) {
                d_img.scroll_to = cataimgui::scroll::page_up;
            } else if( action == "PAGE_UP" ) {
                d_img.scroll_to = cataimgui::scroll::line_up;
            } else if( action == "PAGE_DOWN" ) {
                d_img.scroll_to = cataimgui::scroll::line_down;
            } else if( action == "ANY_INPUT" ) {
                // As create_option_line() shows them.
                const std::string key = right_justify( evt.short_description(), 2 );
                for( size_t i = 0; i < hotkeys.size(); i++ ) {
                    if( hotkeys[i] == key && selectable[i] ) {
                        chosen = static_cast<int>( i );
                    }
                }
            } else if( action == "QUIT" ) {
                chosen = -1;
            }
        }
    }
    if( chosen >= 0 ) {
        history.push_back( { lines[chosen].text, _( "You" ), c_light_blue } );
    }
    return chosen;
}

} // namespace

bool peek( const tripoint_bub_ms &p )
{
    if( !active() ) {
        return false;
    }
    const tripoint_abs_ms at = get_map().get_abs( p );
    net::send_line( to_line( [&]( JsonOut & json ) {
        json.member( "type", "prompt" );
        json.member( "id", 0 );
        json.member( "kind", "peek" );
        json.member( "at" );
        json.start_array();
        json.write( at.x() );
        json.write( at.y() );
        json.write( at.z() );
        json.end_array();
    } ) );
    // What game::peek() costs.
    if( npc *guy = network_npc() ) {
        guy->mod_moves( -guy->get_speed() * 2 );
    }
    return true;
}

std::optional<videogame_result> ask_videogame( const std::string &name )
{
    if( !active() ) {
        return std::nullopt;
    }
    // A game may take a while: the host's game stands still meanwhile.
    const std::optional<std::string> line = ask( "videogame", [&]( JsonOut & json ) {
        json.member( "game", name );
    }, std::chrono::minutes( 60 ) );
    videogame_result result;
    read_answer( line, [&]( const JsonObject & obj ) {
        result.won = obj.get_bool( "won", false );
        result.score = obj.get_int( "score", 0 );
        if( obj.has_object( "data" ) ) {
            for( const JsonMember member : obj.get_object( "data" ) ) {
                result.data[member.name()] = member.get_string();
            }
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
    } else if( kind == "inspect" ) {
        player_talk::inspect( question );
    } else if( kind == "videogame" ) {
        // The game itself on this screen; what came of it goes back.
        std::map<std::string, std::string> data;
        int score = 0;
        const bool won = play_videogame( question.get_string( "game", "" ), data, score );
        send_answer( id, [&]( JsonOut & json ) {
            json.member( "won", won );
            json.member( "score", score );
            json.member( "data", data );
        } );
    } else if( kind == "vehicle" ) {
        remote_vehicle::reopen( question );
    } else if( kind == "trade" ) {
        remote_trade::answer( question );
    } else if( kind == "peek" ) {
        // The game's own peek, on the copy of the map around the character.
        JsonArray at = question.get_array( "at" );
        const int x = at.next_int();
        const int y = at.next_int();
        const int z = at.next_int();
        const tripoint_bub_ms p = get_map().get_bub( tripoint_abs_ms( x, y, z ) );
        if( get_map().inbounds( p ) ) {
            g->peek( p );
        }
    } else if( kind == "dialogue" ) {
        const int ret = answer_dialogue( question );
        send_answer( id, [&]( JsonOut & json ) {
            json.member( "ret", ret );
        } );
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
