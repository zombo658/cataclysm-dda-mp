#include "mp/world_actions.h"

#include <cstdlib>
#include <optional>
#include <sstream>

#include "activity_actor_definitions.h"
#include "character.h"
#include "creature_tracker.h"
#include "gates.h"
#include "item.h"
#include "json.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "mp/net.h"
#include "npc.h"
#include "string_formatter.h"
#include "translations.h"
#include "trap.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace mp::world_actions
{

namespace
{

// handle_action.cpp, open()
std::string open( npc &guy, const tripoint_bub_ms &p )
{
    map &here = get_map();
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        vehicle &veh = vp->vehicle();
        const int openable = veh.next_part_to_open( vp->part_index(), true );
        if( openable < 0 ) {
            return _( "there is nothing to open there" );
        }
        veh.open_all_at( here, openable );
        add_msg( _( "%1$s opens the %2$s's %3$s." ), guy.get_name(), veh.name,
                 veh.part( openable ).name() );
        return std::string();
    }
    if( here.open_door( guy, p, !here.is_outside( guy.pos_bub() ) ) ) {
        add_msg( _( "%1$s opens the %2$s." ), guy.get_name(), here.name( p ) );
        return std::string();
    }
    if( here.has_flag( ter_furn_flag::TFLAG_LOCKED, p ) ) {
        return _( "the door is locked" );
    }
    if( here.ter( p ).obj().close ) {
        return _( "that door is already open" );
    }
    return _( "no door there" );
}

// avatar::smash(), shortened: corpses are pulped, anything else bashed.
std::string smash( npc &guy, const tripoint_bub_ms &p )
{
    map &here = get_map();
    for( const item &maybe_corpse : here.i_at( p ) ) {
        if( maybe_corpse.can_revive() ) {
            guy.assign_activity( pulp_activity_actor( here.get_abs( p ) ) );
            return std::string();
        }
    }
    const bash_params res = here.bash( p, guy.smash_ability() );
    if( !res.did_bash ) {
        return _( "there is nothing to smash there" );
    }
    add_msg( res.success ? _( "%s smashes it." ) : _( "%s whacks it." ), guy.get_name() );
    return std::string();
}

// game::examine() for terrain and furniture.
std::string examine( npc &guy, const tripoint_bub_ms &p )
{
    map &here = get_map();
    const furn_t &xfurn_t = here.furn( p ).obj();
    const ter_t &xter_t = here.ter( p ).obj();
    // trap::iexamine handles the invisible traps.
    here.tr_at( p ).examine( p );
    if( here.has_furn( p ) ) {
        xfurn_t.examine( guy, p );
    } else if( xter_t.can_examine( p ) ) {
        xter_t.examine( guy, p );
    } else if( !here.tr_at( p ).is_null() ) {
        return std::string();
    } else {
        return string_format( _( "There is nothing special about the %s." ), here.name( p ) );
    }
    return std::string();
}

void send_command( const std::string &action, const tripoint_rel_ms &dir )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", "tile_action" );
    json.member( "action", action );
    json.member( "offset" );
    json.start_array();
    json.write( dir.x() );
    json.write( dir.y() );
    json.write( dir.z() );
    json.end_array();
    json.end_object();
    net::client_send_line( os.str() );
}

} // namespace

std::string act( npc &guy, const std::string &action, const tripoint_rel_ms &dir )
{
    if( std::abs( dir.x() ) > 1 || std::abs( dir.y() ) > 1 || std::abs( dir.z() ) > 1 ) {
        return _( "that is too far away" );
    }
    const tripoint_bub_ms p = guy.pos_bub() + dir;
    if( action == "open" ) {
        return open( guy, p );
    } else if( action == "close" ) {
        map &here = get_map();
        const std::string before = here.name( p );
        const ter_id ter_before = here.ter( p );
        const furn_id furn_before = here.furn( p );
        doors::close_door( here, guy, p );
        // close_door() tells only the avatar.
        if( here.ter( p ) != ter_before || here.furn( p ) != furn_before ) {
            add_msg( _( "%1$s closes the %2$s." ), guy.get_name(), before );
        }
        return std::string();
    } else if( action == "smash" ) {
        return smash( guy, p );
    } else if( action == "examine" ) {
        return examine( guy, p );
    }
    return "unknown tile action \"" + action + "\"";
}

bool run( const action_id act )
{
    std::string action;
    std::string question;
    switch( act ) {
        case ACTION_OPEN:
            action = "open";
            question = _( "Open where?" );
            break;
        case ACTION_CLOSE:
            action = "close";
            question = _( "Close where?" );
            break;
        case ACTION_SMASH:
            action = "smash";
            question = _( "Smash where?" );
            break;
        case ACTION_EXAMINE:
        case ACTION_EXAMINE_AND_PICKUP:
            action = "examine";
            question = _( "Examine where?" );
            break;
        default:
            return false;
    }
    if( const std::optional<tripoint_rel_ms> dir = choose_direction( question ) ) {
        send_command( action, *dir );
    }
    return true;
}

} // namespace mp::world_actions
