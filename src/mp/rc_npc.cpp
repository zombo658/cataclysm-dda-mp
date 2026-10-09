#include "mp/rc_npc.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "activity_type.h"
#include "calendar.h"
#include "avatar.h"
#include "character_id.h"
#include "creature.h"
#include "creature_tracker.h"
#include "faction.h"
#include "line.h"
#include "memory_fast.h"
#include "overmapbuffer.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "item_search.h"
#include "json.h"
#include "map.h"
#include "map_selector.h"
#include "mapdata.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/npc_grab.h"
#include "mp/npc_step.h"
#include "mp/protocol.h"
#include "mp/remote_log.h"
#include "mp/remote_prompt.h"
#include "npc.h"
#include "vpart_position.h"
#include "vehicle.h"
#include "player_activity.h"
#include "output.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"
#include "uilist.h"

namespace mp
{

static const faction_id faction_your_followers( "your_followers" );

namespace
{

// IDs of NPCs controlled by the second player. Mirrors the "mp_remote" member
// of the NPC save; npc::load() keeps it in sync for every loaded NPC.
std::set<character_id> remote_ids;

// Set by request_host(), handled on the first turn after loading.
bool host_requested = false;

// True while wait_for_remote_players() runs the host's input.
bool host_blocked = false;

time_mode mode = time_mode::shared;

// Commands are not saved: a save in the middle of a queue drops the queue.
std::map<character_id, std::deque<command>> queues;

} // namespace

bool is_remote( const npc &guy )
{
    return remote_ids.count( guy.getID() ) > 0;
}

bool remote_drives( const map &here, const vehicle &veh )
{
    for( const npc &guy : g->all_npcs() ) {
        if( is_remote( guy ) && guy.controlling_vehicle && veh.player_in_control( here, guy ) ) {
            return true;
        }
    }
    return false;
}

bool is_remote_character( const Character &who )
{
    const npc *guy = who.as_npc();
    return guy != nullptr && is_remote( *guy );
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

npc *network_npc()
{
    for( npc &guy : g->all_npcs() ) {
        if( is_remote( guy ) ) {
            return &guy;
        }
    }
    return nullptr;
}

static void reject( const npc &guy, const std::string &why )
{
    add_msg( m_bad, _( "%1$s can't do that: %2$s" ), guy.get_name(), why );
    protocol::send_rejected( why );
}

// game::start_hauling() for the remote character: the hauled items follow.
static void start_hauling( npc &guy, const tripoint_bub_ms &from )
{
    map &here = get_map();
    std::vector<item_location> candidate_items = here.get_haulable_items( from );
    guy.trim_haul_list( candidate_items );
    std::vector<item_location> target_items = guy.haul_list;
    if( guy.is_autohauling() && !guy.suppress_autohaul ) {
        for( const item_location &it : guy.haul_list ) {
            candidate_items.erase( std::remove( candidate_items.begin(), candidate_items.end(), it ),
                                   candidate_items.end() );
        }
        if( guy.hauling_filter.empty() ) {
            target_items.insert( target_items.end(), candidate_items.begin(), candidate_items.end() );
        } else {
            const std::function<bool( const item & )> filter = item_filter_from_string( guy.hauling_filter );
            std::copy_if( candidate_items.begin(), candidate_items.end(), std::back_inserter( target_items ),
            [&filter]( const item_location & it ) {
                return filter( *it );
            } );
        }
    }
    guy.suppress_autohaul = false;
    guy.haul_list.clear();
    if( target_items.empty() ) {
        if( !guy.is_autohauling() ) {
            guy.stop_hauling();
        }
        return;
    }
    const std::vector<int> quantities( target_items.size(), 0 );
    guy.assign_activity( move_items_activity_actor( target_items, quantities, false,
                         tripoint_rel_ms(), true ) );
}

static void do_move( npc &guy, const point_rel_ms &dir )
{
    map &here = get_map();
    if( guy.controlling_vehicle ) {
        // Steering, as the host's pldrive() does for the avatar.
        if( const optional_vpart_position vp = here.veh_at( guy.pos_bub( here ) ) ) {
            vehicle &veh = vp->vehicle();
            if( !veh.is_flying_in_air() && !veh.can_control_on_land( guy ) ) {
                reject( guy, _( "You have no idea how to make the vehicle move." ) );
                return;
            }
            veh.pldrive( here, guy, dir.x(), dir.y() );
            return;
        }
        guy.controlling_vehicle = false;
    }
    const tripoint_bub_ms dest = guy.pos_bub( here ) + dir;
    if( !here.inbounds( dest ) ) {
        // TODO: decide what happens at the edge of the reality bubble.
        reject( guy, _( "too far from the host" ) );
        return;
    }
    // Pushing a grabbed thing goes into its tile, game::walk_move() order.
    const tripoint_rel_ms dp( dir, 0 );
    bool may_enter = false;
    npc_grab::prepare_step( guy, dp, may_enter );
    if( npc_grab::type( guy ) != object_type::NONE && !may_enter && here.impassable( dest ) &&
        get_creature_tracker().creature_at( dest ) == nullptr ) {
        return;
    }
    if( npc_grab::drag( guy, dp ) ) {
        return;
    }
    const tripoint_bub_ms old_pos = guy.pos_bub( here );
    const FacingDirection facing = guy.facing;
    // The host's way of stepping; the AI's npc::move_to() only for what it
    // doesn't cover (ramps, being stunned).
    if( !npc_step::step( guy, dp ) ) {
        // The follower's rules would close and lock doors behind the player.
        const npc_follower_rules rules = guy.rules;
        for( const ally_rule rule : { ally_rule::close_doors, ally_rule::lock_doors } ) {
            guy.rules.clear_flag( rule );
            guy.rules.disable_override( rule );
        }
        guy.move_to( dest, true );
        guy.rules = rules;
    }
    // As avatar_action::move(): a step up or down keeps the facing.
    if( dir.x() == 0 ) {
        guy.facing = facing;
    } else {
        guy.facing = dir.x() > 0 ? FacingDirection::RIGHT : FacingDirection::LEFT;
    }
    if( guy.is_hauling() && guy.pos_bub( here ) != old_pos ) {
        start_hauling( guy, old_pos );
    }
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
    protocol::mark_changed();
    if( guy.activity ) {
        // Whatever the activity asks (the shape of an installed part, ...)
        // asks the second player.
        remote_prompt::asking_client asking;
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

bool instant_mode()
{
    return net::running() && mode == time_mode::instant;
}

bool shared_time()
{
    return net::running() && mode == time_mode::shared;
}

bool busy( const npc &guy )
{
    return guy.get_moves() <= 0 || guy.activity || has_commands( guy ) || guy.in_sleep_state();
}

bool remote_needs_time()
{
    if( !shared_time() || !net::has_client() ) {
        return false;
    }
    const npc *guy = network_npc();
    return guy != nullptr && !guy->is_dead() && ( busy( *guy ) || protocol::has_deferred() );
}

int host_input_timeout()
{
    return shared_time() && net::has_client() ? 30 : 125;
}

// Shared time, when one player is busy with something long (an activity,
// sleep) and the other is not: the free one's actions move time on, and
// while they do nothing it still moves, at this pace. When both are busy,
// time runs as fast as the game can go.
static constexpr std::chrono::milliseconds idle_turn( 1000 );

static bool host_busy()
{
    const avatar &u = get_avatar();
    return static_cast<bool>( u.activity ) || u.in_sleep_state();
}

// The second player's character is busy with something long.
static bool partner_busy( const npc &guy )
{
    return static_cast<bool>( guy.activity ) || guy.in_sleep_state();
}

// Said once per stretch of the other player's long action.
static bool told_partner_host_busy = false;
static bool told_host_partner_busy = false;

// At the start of a turn in shared time.
static void shared_turn_start()
{
    npc *guy = network_npc();
    if( guy == nullptr ) {
        return;
    }
    // Moves don't pile up while the second player stands still, as the
    // avatar's don't: at most a turn's worth to spend before monmove() gives
    // the next one.
    const int turn_worth = std::max( guy->get_speed(), 100 );
    if( guy->get_moves() > turn_worth ) {
        guy->set_moves( turn_worth );
    }
    if( !host_busy() ) {
        told_partner_host_busy = false;
    }
    if( !partner_busy( *guy ) ) {
        told_host_partner_busy = false;
    }
}

// The host is busy with something long and the second player is not: the
// turn waits for the second player to act, at most idle_turn.
static void let_partner_act( npc &guy, const std::function<void()> &host_keys )
{
    if( !told_partner_host_busy ) {
        told_partner_host_busy = true;
        remote_log::to_second_player( guy, string_format(
                                          _( "%s is busy.  Time goes on as you act (at least a turn a second); "
                                             "skip turns with wait (.) or wait longer (|)." ), get_avatar().get_name() ), m_info );
    }
    protocol::send_state( guy );
    // The host sees time going at this pace, not a frozen screen.
    g->invalidate_main_ui_adaptor();
    ui_manager::redraw();
    refresh_display();
    const auto start = std::chrono::steady_clock::now();
    auto keys_checked = start;
    while( std::chrono::steady_clock::now() - start < idle_turn ) {
        protocol::poll();
        if( busy( guy ) || protocol::has_deferred() || !host_busy() || !net::has_client() ) {
            // The second player acted (or the host stopped): the turn goes on.
            return;
        }
        // The host can still stop their activity, as while it runs alone
        // (do_turn() looks at the keys ten times a second too).
        if( std::chrono::steady_clock::now() - keys_checked > std::chrono::milliseconds( 100 ) ) {
            keys_checked = std::chrono::steady_clock::now();
            host_keys();
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
}

// Activities that are about time passing (sleeping, waiting): they take the
// host's time, a turn per turn, even when the second player's time is not
// counted.
static bool takes_real_time( const player_activity &act )
{
    static const std::set<std::string> real_time = {
        "ACT_TRY_SLEEP", "ACT_WAIT", "ACT_WAIT_WEATHER", "ACT_WAIT_NPC", "ACT_WAIT_STAMINA",
        "ACT_WAIT_FOLLOWERS"
    };
    return real_time.count( act.id().str() ) > 0;
}

void run_instantly( npc &guy )
{
    if( !is_remote( guy ) ) {
        return;
    }
    if( guy.activity && takes_real_time( guy.activity ) ) {
        // One turn of it now; the next on the host's next turn.
        if( guy.get_moves() > 0 ) {
            remote_move( guy );
        }
        guy.set_moves( 0 );
        return;
    }
    // The second player's time is not counted: give the NPC whatever it needs
    // for every step, so that commands and the activities they start (picking
    // up, ...) finish at once. The cap only guards against an activity that
    // never ends.
    // 200000 one-second steps cover even a craft of two days.
    for( int steps = 0; steps < 200000 && ( has_commands( guy ) || guy.activity ) &&
         !( guy.activity && takes_real_time( guy.activity ) ); steps++ ) {
        guy.set_moves( std::max( guy.get_speed(), 100 ) );
        remote_move( guy );
    }
    guy.set_moves( 0 );
    g->invalidate_main_ui_adaptor();
}

bool waits_for_commands( const npc &guy )
{
    return is_remote( guy ) && !guy.activity && !has_commands( guy );
}

static npc *first_waiting_npc()
{
    for( npc &guy : g->all_npcs() ) {
        // Trying to fall asleep counts as asleep, but the activity has to go on.
        if( is_remote( guy ) && !guy.is_dead() && ( !guy.in_sleep_state() || guy.activity ) &&
            guy.get_moves() > 0 ) {
            return &guy;
        }
    }
    return nullptr;
}

static void start_hosting();

// No client is connected to the running server: nobody can give the order.
static bool nobody_to_ask()
{
    return net::running() && !net::has_client();
}

void wait_for_remote_players( const std::function<bool()> &host_input,
                              const std::function<void()> &host_keys )
{
    if( host_requested ) {
        host_requested = false;
        start_hosting();
    }
    protocol::send_state_if_due();
    if( shared_time() ) {
        protocol::poll();
        shared_turn_start();
        npc *partner = network_npc();
        if( partner != nullptr && net::has_client() && host_busy() && !partner->is_dead() &&
            !busy( *partner ) ) {
            let_partner_act( *partner, host_keys );
        }
        // The world doesn't wait: what the character has to do happens in
        // monmove(), its new commands as soon as it has moves again.
        return;
    }
    bool announced = false;
    // Tell the client once per state it has to act in.
    const npc *notified_npc = nullptr;
    int notified_moves = 0;
    for( ;; ) {
        protocol::poll();
        npc *guy = first_waiting_npc();
        if( guy == nullptr ) {
            break;
        }
        if( instant_mode() ) {
            // The world never waits for the second player; their commands run
            // as they arrive (see run_instantly()).
            run_instantly( *guy );
            continue;
        }
        if( !waits_for_commands( *guy ) ) {
            const int moves_before = guy->get_moves();
            guy->move();
            if( guy->get_moves() == moves_before && !waits_for_commands( *guy ) ) {
                // An activity that does not spend moves; don't spin forever.
                guy->set_moves( 0 );
            }
            continue;
        }
        if( nobody_to_ask() ) {
            // TODO: open question, what happens when the client is away. For
            // now the character stands still and the game goes on.
            guy->pause();
            continue;
        }
        if( net::has_client() && ( notified_npc != guy || notified_moves != guy->get_moves() ) ) {
            protocol::send_your_turn( *guy );
            notified_npc = guy;
            notified_moves = guy->get_moves();
        }
        if( !announced ) {
            add_msg( m_info, _( "Waiting for %s to act…" ), guy->get_name() );
            announced = true;
            // Auto-move would feed the host's own steps to the remote NPC.
            get_avatar().clear_destination();
        }
        g->wait_popup_reset();
        ui_manager::redraw();
        host_blocked = true;
        const bool game_over = host_input();
        host_blocked = false;
        if( game_over ) {
            return;
        }
    }
}

bool host_input_should_yield()
{
    // Keep the connection alive while the host thinks, too, and the client's
    // copy up to date: what the second player's last action changed may come
    // after the last state was sent (sending is limited to a few a second).
    protocol::poll();
    protocol::send_pending_state();
    if( remote_needs_time() ) {
        avatar &u = get_avatar();
        const npc *guy = network_npc();
        if( guy != nullptr && partner_busy( *guy ) ) {
            // Something long of the second player's: the host plays on and
            // their actions move time; standing still, a turn a second.
            if( !told_host_partner_busy ) {
                told_host_partner_busy = true;
                add_msg( m_info, _( "%s is busy.  Time goes on as you act (at least a turn a second); "
                                    "wait (.) or wait longer (|) to skip time." ), guy->get_name() );
            }
            static std::chrono::steady_clock::time_point last_idle_turn;
            const auto now = std::chrono::steady_clock::now();
            if( now - last_idle_turn < idle_turn ) {
                return false;
            }
            last_idle_turn = now;
        }
        // The host stands still: their avatar waits, and the turn passes.
        u.pause();
        return true;
    }
    if( !host_blocked ) {
        return false;
    }
    const npc *guy = first_waiting_npc();
    return guy == nullptr || !waits_for_commands( *guy ) || nobody_to_ask();
}

bool intercept_host_action( const action_id act )
{
    if( !host_blocked || !can_action_change_worldstate( act ) ) {
        return false;
    }
    npc *guy = first_waiting_npc();
    if( guy == nullptr ) {
        return false;
    }
    if( net::running() ) {
        add_msg( m_info, _( "Waiting for the second player (%s)." ), guy->get_name() );
        return true;
    }
    command cmd;
    switch( act ) {
        case ACTION_MOVE_FORTH:
        case ACTION_MOVE_FORTH_RIGHT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_BACK_RIGHT:
        case ACTION_MOVE_BACK:
        case ACTION_MOVE_BACK_LEFT:
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_FORTH_LEFT:
            cmd.type = command_type::move;
            cmd.dir = get_delta_from_movement_action( act, iso_rotate::yes );
            break;
        case ACTION_PAUSE:
            cmd.type = command_type::wait;
            break;
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL:
            cmd.type = command_type::pickup_all;
            break;
        default:
            add_msg( m_info, _( "Waiting for %s.  Only actions that take no time are allowed." ),
                     guy->get_name() );
            return true;
    }
    push_command( *guy, cmd );
    return true;
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

static void make_partner( npc &guy )
{
    if( !guy.is_player_ally() ) {
        // The second player is on the host's side.
        guy.set_fac( faction_your_followers );
        guy.set_attitude( NPCATT_FOLLOW );
        g->add_npc_follower( guy.getID() );
    }
    set_remote( guy, true );
}

// A new random character next to the host, for the second player.
static npc *spawn_partner()
{
    map &here = get_map();
    const avatar &host = get_avatar();
    std::optional<tripoint_bub_ms> spot;
    for( const tripoint_bub_ms &p : closest_points_first( host.pos_bub( here ), 1, 5 ) ) {
        if( here.passable( p ) && g->is_empty( p ) ) {
            spot = p;
            break;
        }
    }
    if( !spot ) {
        popup( _( "There is no free place for a new character next to you." ) );
        return nullptr;
    }
    shared_ptr_fast<npc> guy = make_shared_fast<npc>();
    guy->normalize();
    guy->randomize();
    guy->spawn_at_precise( here.get_abs( *spot ) );
    overmap_buffer.insert_npc( guy );
    guy->mission = NPC_MISSION_NULL;
    guy->set_fac( faction_your_followers );
    guy->set_attitude( NPCATT_FOLLOW );
    g->load_npcs();
    g->add_npc_follower( guy->getID() );
    return guy.get();
}

static void choose_partner()
{
    std::vector<npc *> nearby;
    uilist menu;
    menu.text = _( "Who will the second player play?" );
    for( npc &guy : g->all_npcs() ) {
        menu.addentry( static_cast<int>( nearby.size() ), true, MENU_AUTOASSIGN, "%s%s",
                       guy.get_name(), guy.is_player_ally() ? _( " (your follower)" ) : "" );
        nearby.push_back( &guy );
    }
    const int new_character = static_cast<int>( nearby.size() );
    menu.addentry( new_character, true, 'n', _( "A new character" ) );
    menu.query();
    npc *chosen = nullptr;
    if( menu.ret == new_character ) {
        chosen = spawn_partner();
    } else if( menu.ret >= 0 && menu.ret < new_character ) {
        chosen = nearby[menu.ret];
    }
    if( chosen != nullptr ) {
        make_partner( *chosen );
        add_msg( m_info, _( "The second player will play %s." ), chosen->get_name() );
    }
}

void request_host()
{
    host_requested = true;
}

// How the second player's time counts, asked when hosting.
static void choose_time_mode()
{
    uilist menu;
    menu.text = _( "How does time pass for the second player?" );
    menu.addentry_desc( static_cast<int>( time_mode::shared ), true, 's', _( "Shared time" ),
                        _( "Each player's actions take game time, as in the single player game.  "
                           "Whoever acts moves time on; the one who stands still waits.  While you "
                           "don't press anything, your character waits for the second player." ) );
    menu.addentry_desc( static_cast<int>( time_mode::instant ), true, 'i', _( "Instant" ),
                        _( "The second player's actions take no game time at all; the game never "
                           "waits for them.  Fast, but they can outrun anything." ) );
    menu.desc_enabled = true;
    menu.selected = static_cast<int>( mode );
    menu.query();
    if( menu.ret == static_cast<int>( time_mode::instant ) ) {
        mode = time_mode::instant;
    } else if( menu.ret == static_cast<int>( time_mode::shared ) ) {
        mode = time_mode::shared;
    }
}

static void start_hosting()
{
    choose_time_mode();
    std::string error;
    if( !net::start( net::default_port, error ) ) {
        popup( _( "Can't start the multiplayer server on port %1$d: %2$s" ), net::default_port, error );
        return;
    }
    if( network_npc() == nullptr ) {
        choose_partner();
    }
    const npc *partner = network_npc();
    // In the log, not a popup: a popup would hold the network until closed.
    add_msg( m_info, _( "The game is hosted on port %1$d.  The second player chooses "
                        "Multiplayer > Join game in the main menu and enters your IP address "
                        "(ipconfig shows it; over the internet use a virtual network like Radmin VPN "
                        "or forward TCP port %1$d)." ), net::default_port );
    add_msg( m_info, partner != nullptr ?
             string_format( _( "They will play %s." ), partner->get_name() ) :
             _( "No character is chosen for them yet." ) );
}

static void toggle_remote_menu()
{
    npc *guy = pick_npc( _( "Toggle remote control for which NPC?" ), false );
    if( guy == nullptr ) {
        return;
    }
    if( is_remote( *guy ) ) {
        set_remote( *guy, false );
    } else {
        make_partner( *guy );
    }
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

static void toggle_server()
{
    if( net::running() ) {
        net::stop();
        add_msg( m_info, _( "Multiplayer server stopped." ) );
        return;
    }
    std::string error;
    if( net::start( net::default_port, error ) ) {
        add_msg( m_info, _( "Multiplayer server is listening on port %d." ), net::default_port );
    } else {
        popup( _( "Can't start the multiplayer server on port %1$d: %2$s" ), net::default_port, error );
    }
}

void debug_menu()
{
    uilist menu;
    menu.text = _( "Multiplayer" );
    menu.addentry( 0, true, 't', _( "Toggle remote control of an NPC" ) );
    menu.addentry( 1, true, 'q', _( "Queue a command for a remote NPC" ) );
    menu.addentry( 2, true, 's', net::running() ? _( "Stop the server" ) :
                   string_format( _( "Start the server (port %d)" ), net::default_port ) );
    menu.query();
    switch( menu.ret ) {
        case 0:
            toggle_remote_menu();
            break;
        case 1:
            queue_command_menu();
            break;
        case 2:
            toggle_server();
            break;
        default:
            break;
    }
}

} // namespace mp
