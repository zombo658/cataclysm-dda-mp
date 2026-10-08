#include "mp/rc_npc.h"

#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "character_id.h"
#include "creature.h"
#include "creature_tracker.h"
#include "faction.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "map.h"
#include "map_selector.h"
#include "mapdata.h"
#include "messages.h"
#include "npc.h"
#include "output.h"
#include "translations.h"
#include "uilist.h"

namespace mp
{

static const faction_id faction_your_followers( "your_followers" );

namespace
{

// IDs of NPCs controlled by the second player. Mirrors the "mp_remote" member
// of the NPC save; npc::load() keeps it in sync for every loaded NPC.
std::set<character_id> remote_ids;

// Commands are not saved: a save in the middle of a queue drops the queue.
std::map<character_id, std::deque<command>> queues;

} // namespace

bool is_remote( const npc &guy )
{
    return remote_ids.count( guy.getID() ) > 0;
}

void set_remote( npc &guy, const bool remote )
{
    if( remote ) {
        remote_ids.insert( guy.getID() );
        // Drop whatever the AI was doing.
        guy.path.clear();
    } else {
        remote_ids.erase( guy.getID() );
        queues.erase( guy.getID() );
    }
}

void push_command( const npc &guy, const command &cmd )
{
    queues[guy.getID()].push_back( cmd );
}

bool has_commands( const npc &guy )
{
    const auto it = queues.find( guy.getID() );
    return it != queues.end() && !it->second.empty();
}

static void reject( const npc &guy, const std::string &why )
{
    add_msg( m_bad, _( "%1$s can't do that: %2$s" ), guy.get_name(), why );
}

static void do_move( npc &guy, const point_rel_ms &dir )
{
    map &here = get_map();
    const tripoint_bub_ms dest = guy.pos_bub( here ) + dir;
    if( !here.inbounds( dest ) ) {
        // TODO: decide what happens at the edge of the reality bubble.
        reject( guy, _( "too far from the host" ) );
        return;
    }
    Creature *critter = get_creature_tracker().creature_at( dest );
    if( critter != nullptr && critter != &guy &&
        guy.attitude_to( *critter ) == Creature::Attitude::HOSTILE ) {
        guy.melee_attack( *critter, true );
        return;
    }
    // npc::move_to() would spend the whole turn bumping into a wall.
    if( here.impassable( dest ) && !here.has_flag( ter_furn_flag::TFLAG_DOOR, dest ) &&
        !here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_CLIMBABLE, dest ) ) {
        reject( guy, _( "the way is blocked" ) );
        return;
    }
    guy.move_to( dest, true );
}

static void do_attack( npc &guy, const point_rel_ms &dir )
{
    const tripoint_bub_ms dest = guy.pos_bub() + dir;
    Creature *critter = get_creature_tracker().creature_at( dest );
    if( critter == nullptr || critter == &guy ) {
        reject( guy, _( "nothing to attack there" ) );
        return;
    }
    guy.melee_attack( *critter, true );
}

static void do_pickup_all( npc &guy )
{
    map &here = get_map();
    const tripoint_bub_ms pos = guy.pos_bub( here );
    drop_locations what;
    for( item &it : here.i_at( pos ) ) {
        what.emplace_back( item_location( map_cursor( pos ), &it ), it.count() );
    }
    if( what.empty() ) {
        reject( guy, _( "nothing to pick up" ) );
        return;
    }
    // Starts a pickup activity; remote_move() keeps it running.
    guy.pick_up( what );
}

static void execute( npc &guy, const command &cmd )
{
    switch( cmd.type ) {
        case command_type::move:
            do_move( guy, cmd.dir );
            break;
        case command_type::attack:
            do_attack( guy, cmd.dir );
            break;
        case command_type::wait:
            guy.pause();
            break;
        case command_type::pickup_all:
            do_pickup_all( guy );
            break;
    }
}

bool remote_move( npc &guy )
{
    if( !is_remote( guy ) ) {
        return false;
    }
    if( guy.activity ) {
        guy.do_player_activity();
        return true;
    }
    std::deque<command> &queue = queues[guy.getID()];
    // Skip rejected commands, so that every call either spends moves or
    // empties the queue. Otherwise monmove() takes the NPC for an AI stuck in
    // a loop and makes it faint.
    while( !queue.empty() ) {
        const command cmd = queue.front();
        queue.pop_front();
        const int moves_before = guy.get_moves();
        execute( guy, cmd );
        if( guy.get_moves() != moves_before || guy.activity ) {
            break;
        }
    }
    return true;
}

bool waits_for_commands( const npc &guy )
{
    return is_remote( guy ) && !guy.activity && !has_commands( guy );
}

void store_npc( const npc &guy, JsonOut &json )
{
    if( is_remote( guy ) ) {
        json.member( "mp_remote", true );
    }
}

void load_npc( npc &guy, const JsonObject &data )
{
    bool remote = false;
    data.read( "mp_remote", remote );
    set_remote( guy, remote );
}

static npc *pick_npc( const std::string &title, const bool remote_only )
{
    std::vector<npc *> candidates;
    uilist menu;
    menu.text = title;
    for( npc &guy : g->all_npcs() ) {
        if( remote_only && !is_remote( guy ) ) {
            continue;
        }
        menu.addentry( static_cast<int>( candidates.size() ), true, MENU_AUTOASSIGN, "%s%s",
                       guy.get_name(), is_remote( guy ) ? _( " (remote)" ) : "" );
        candidates.push_back( &guy );
    }
    if( candidates.empty() ) {
        popup( remote_only ? _( "There are no remote NPCs nearby." ) :
               _( "There are no NPCs nearby." ) );
        return nullptr;
    }
    menu.query();
    if( menu.ret < 0 || static_cast<size_t>( menu.ret ) >= candidates.size() ) {
        return nullptr;
    }
    return candidates[menu.ret];
}

static void toggle_remote_menu()
{
    npc *guy = pick_npc( _( "Toggle remote control for which NPC?" ), false );
    if( guy == nullptr ) {
        return;
    }
    if( !is_remote( *guy ) && !guy->is_player_ally() ) {
        // The second player is on the host's side.
        guy->set_fac( faction_your_followers );
        guy->set_attitude( NPCATT_FOLLOW );
        g->add_npc_follower( guy->getID() );
    }
    set_remote( *guy, !is_remote( *guy ) );
    add_msg( m_info, is_remote( *guy ) ? _( "%s is now controlled remotely." ) :
             _( "%s is now controlled by the AI." ), guy->get_name() );
}

static std::optional<point_rel_ms> pick_direction()
{
    static const std::vector<std::pair<std::string, point_rel_ms>> dirs = {
        { translate_marker( "north" ), point_rel_ms::north },
        { translate_marker( "north-east" ), point_rel_ms::north_east },
        { translate_marker( "east" ), point_rel_ms::east },
        { translate_marker( "south-east" ), point_rel_ms::south_east },
        { translate_marker( "south" ), point_rel_ms::south },
        { translate_marker( "south-west" ), point_rel_ms::south_west },
        { translate_marker( "west" ), point_rel_ms::west },
        { translate_marker( "north-west" ), point_rel_ms::north_west },
    };
    uilist menu;
    menu.text = _( "Direction?" );
    for( size_t i = 0; i < dirs.size(); i++ ) {
        menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, _( dirs[i].first ) );
    }
    menu.query();
    if( menu.ret < 0 || static_cast<size_t>( menu.ret ) >= dirs.size() ) {
        return std::nullopt;
    }
    return dirs[menu.ret].second;
}

static void queue_command_menu()
{
    npc *guy = pick_npc( _( "Queue a command for which NPC?" ), true );
    if( guy == nullptr ) {
        return;
    }
    uilist menu;
    menu.text = string_format( _( "Command for %s" ), guy->get_name() );
    menu.addentry( 0, true, 'm', _( "Move" ) );
    menu.addentry( 1, true, 'a', _( "Attack" ) );
    menu.addentry( 2, true, 'w', _( "Wait" ) );
    menu.addentry( 3, true, 'g', _( "Pick up everything here" ) );
    menu.query();
    command cmd;
    switch( menu.ret ) {
        case 0:
        case 1: {
            const std::optional<point_rel_ms> dir = pick_direction();
            if( !dir ) {
                return;
            }
            cmd.type = menu.ret == 0 ? command_type::move : command_type::attack;
            cmd.dir = *dir;
            break;
        }
        case 2:
            cmd.type = command_type::wait;
            break;
        case 3:
            cmd.type = command_type::pickup_all;
            break;
        default:
            return;
    }
    push_command( *guy, cmd );
}

void debug_menu()
{
    uilist menu;
    menu.text = _( "Multiplayer" );
    menu.addentry( 0, true, 't', _( "Toggle remote control of an NPC" ) );
    menu.addentry( 1, true, 'q', _( "Queue a command for a remote NPC" ) );
    menu.query();
    switch( menu.ret ) {
        case 0:
            toggle_remote_menu();
            break;
        case 1:
            queue_command_menu();
            break;
        default:
            break;
    }
}

} // namespace mp
