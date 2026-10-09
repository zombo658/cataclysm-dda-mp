#include "mp/player_talk.h"

#include <sstream>

#include "avatar.h"
#include "bodygraph.h"
#include "character.h"
#include "creature_tracker.h"
#include "game.h"
#include "json.h"
#include "map.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/npc_step.h"
#include "mp/rc_npc.h"
#include "mp/remote_log.h"
#include "mp/remote_trade.h"
#include "npc.h"
#include "output.h"
#include "proficiency.h"
#include "skill.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "uilist.h"

static const proficiency_id proficiency_prof_wound_care( "prof_wound_care" );
static const proficiency_id proficiency_prof_wound_care_expert( "prof_wound_care_expert" );

static const skill_id skill_firstaid( "firstaid" );

namespace mp::player_talk
{

namespace
{

void send_chat( const std::string &from, const std::string &text )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "type", "chat" );
    json.member( "from", from );
    json.member( "text", text );
    json.end_object();
    net::send_line( os.str() );
}

// The line in the host's log; the client gets it as "chat", not again with
// the log.
void host_log( const std::string &from, const std::string &text )
{
    const std::string line = string_format( _( "%1$s: %2$s" ), from, text );
    remote_log::told_already( line );
    add_msg( m_info, "%s", line );
}

// Asks the client to show the host's wounds or status on its copy.
void ask_inspect( const std::string &what )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "type", "prompt" );
    json.member( "id", 0 );
    json.member( "kind", "inspect" );
    json.member( "what", what );
    json.member( "at", get_avatar().pos_abs() );
    json.end_object();
    net::send_line( os.str() );
}

} // namespace

void host_menu( npc &guy )
{
    avatar &host = get_avatar();
    enum choices : int { say = 0, swap_pos, trade, wounds, status };
    uilist amenu;
    amenu.text = string_format( _( "What to do with %s?" ), host.get_name() );
    amenu.addentry( say, true, 't', _( "Say something" ) );
    amenu.addentry( swap_pos, !guy.is_mounted() && !host.is_mounted() &&
                    rl_dist( guy.pos_bub(), host.pos_bub() ) == 1, 's', _( "Swap positions" ) );
    amenu.addentry( trade, rl_dist( guy.pos_bub(), host.pos_bub() ) <= 1, 'b', _( "Trade" ) );
    amenu.addentry( wounds, true, 'w', _( "Examine wounds" ) );
    amenu.addentry( status, true, 'e', _( "Examine status" ) );
    amenu.query();
    switch( amenu.ret ) {
        case say: {
            const std::string text = string_input_popup()
                                     .title( string_format( _( "Say to %s:" ), host.get_name() ) )
                                     .width( 60 )
                                     .query_string();
            if( !text.empty() ) {
                say_from_client( guy, text );
            }
            break;
        }
        case swap_pos:
            if( query_yn( _( "Swap places with %s?" ), host.get_name() ) ) {
                guy.add_msg_if_player( _( "You swap places with %s." ), host.get_name() );
                add_msg( _( "%s swaps places with you." ), guy.get_name() );
                g->swap_critters( host, guy );
                guy.mod_moves( -200 );
            }
            break;
        case trade:
            remote_trade::trade_with_host( guy );
            break;
        case wounds:
            ask_inspect( "wounds" );
            break;
        case status:
            ask_inspect( "status" );
            break;
        default:
            break;
    }
}

void say_from_client( const npc &guy, const std::string &text )
{
    // Not spoken in the game's world: nobody else hears it.
    host_log( guy.get_name(), text );
    send_chat( guy.get_name(), text );
}

bool host_talks_to( const npc &who )
{
    if( !is_remote( who ) || !net::has_client() ) {
        return false;
    }
    const std::string text = string_input_popup()
                             .title( string_format( _( "Say to %s:" ), who.get_name() ) )
                             .width( 60 )
                             .query_string();
    if( !text.empty() ) {
        host_log( get_avatar().get_name(), text );
        send_chat( get_avatar().get_name(), text );
    }
    return true;
}

void show( const JsonObject &message )
{
    message.allow_omitted_members();
    // Both players' words, in the log of this screen.
    add_msg( m_info, _( "%1$s: %2$s" ), message.get_string( "from", "" ),
             message.get_string( "text", "" ) );
}

void inspect( const JsonObject &question )
{
    tripoint_abs_ms at;
    question.read( "at", at );
    npc *who = get_creature_tracker().creature_at<npc>( at );
    if( who == nullptr ) {
        popup( _( "You can't see them." ) );
        return;
    }
    if( question.get_string( "what", "" ) == "status" ) {
        display_bodygraph( *who );
        return;
    }
    // game::npc_menu(), "Examine wounds", with this player's skill.
    const avatar &u = get_avatar();
    float prof_bonus = u.get_skill_level( skill_firstaid );
    prof_bonus = u.has_proficiency( proficiency_prof_wound_care ) ? prof_bonus + 1 : prof_bonus;
    prof_bonus = u.has_proficiency( proficiency_prof_wound_care_expert ) ? prof_bonus + 2 : prof_bonus;
    const bool precise = prof_bonus * 4 + u.per_cur >= 20;
    who->body_window( _( "Limbs of: " ) + who->disp_name(), true, precise, 0, 0, 0,
                                          0.0f, 0.0f, 0.0f, 0.0f, 0.0f );
}

} // namespace mp::player_talk
