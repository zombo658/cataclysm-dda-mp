#include "mp/remote_log.h"

#include <deque>
#include <sstream>

#include "character.h"
#include "json.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/rc_npc.h"
#include "mp/remote_actions.h"

namespace mp::remote_log
{

namespace
{

// Recent lines that the client must not get from the host's log.
std::deque<std::string> not_shared;

void remember( const std::string &text )
{
    if( text.empty() ) {
        return;
    }
    not_shared.push_back( text );
    while( not_shared.size() > 200 ) {
        not_shared.pop_front();
    }
}

} // namespace

bool to_second_player( const Character &who, const std::string &text, const game_message_type type )
{
    if( !is_remote_character( who ) ) {
        return false;
    }
    if( net::has_client() && !text.empty() ) {
        std::ostringstream os;
        JsonOut json( os );
        json.start_object();
        json.member( "type", "personal" );
        json.member( "text", text );
        json.member( "kind", static_cast<int>( type ) );
        json.end_object();
        net::send_line( os.str() );
    }
    return true;
}

void told_already( const std::string &text )
{
    remember( text );
}

void host_only( const Character &who, const std::string &text )
{
    // On the client its own copy is the avatar too: nothing is passed on there.
    if( !remote_actions::client_active() && who.is_avatar() ) {
        remember( text );
    }
}

bool is_shared( const std::string &line )
{
    for( const std::string &text : not_shared ) {
        // The log adds " x 2" to repeated lines.
        if( line.compare( 0, text.size(), text ) == 0 ) {
            return false;
        }
    }
    return true;
}

void read( const JsonObject &message )
{
    message.allow_omitted_members();
    Messages::add_msg( static_cast<game_message_type>( message.get_int( "kind", m_neutral ) ),
                       message.get_string( "text", "" ) );
}

} // namespace mp::remote_log
