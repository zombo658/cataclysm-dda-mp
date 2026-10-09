#include "mp/world_actions.h"

#include <cstdlib>
#include <optional>
#include <sstream>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "character.h"
#include "creature_tracker.h"
#include "game.h"
#include "veh_type.h"
#include "vpart_range.h"
#include "sounds.h"
#include "rng.h"
#include "output.h"
#include "item_group.h"
#include "field_type.h"
#include "field.h"
#include "event_bus.h"
#include "event.h"
#include "calendar.h"
#include "bodypart.h"
#include "gates.h"
#include "line.h"
#include "item.h"
#include "json.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/npc_grab.h"
#include "npc.h"
#include "string_formatter.h"
#include "translations.h"
#include "trap.h"
#include "vehicle.h"
#include "vpart_position.h"

static const skill_id skill_melee( "melee" );

namespace mp::world_actions
{

namespace
{

// handle_action.cpp, open()
std::string open( npc &guy, const tripoint_bub_ms &p )
{
    map &here = get_map();
    guy.mod_moves( -to_moves<int>( 1_seconds ) );
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        vehicle *const veh = &vp->vehicle();
        if( !veh->handle_potential_theft( guy ) ) {
            guy.mod_moves( to_moves<int>( 1_seconds ) );
            return std::string();
        }
        const int openable = veh->next_part_to_open( vp->part_index() );
        if( openable >= 0 ) {
            // From inside the vehicle anything opens (curtains too), from
            // outside only what opens from outside.
            const vehicle *own_veh = veh_pointer_or_null( here.veh_at( guy.pos_bub() ) );
            const std::string part_name = veh->part( openable ).name();
            if( own_veh == veh ) {
                veh->open( here, openable );
                guy.add_msg_if_player( _( "You open the %1$s's %2$s." ), veh->name, part_name );
            } else if( veh->next_part_to_open( vp->part_index(), true ) == -1 ) {
                guy.mod_moves( to_moves<int>( 1_seconds ) );
                return string_format( _( "That %s can only be opened from the inside." ), part_name );
            } else {
                veh->open_all_at( here, openable );
                guy.add_msg_if_player( _( "You open the %1$s's %2$s." ), veh->name, part_name );
            }
            return std::string();
        }
        guy.mod_moves( to_moves<int>( 1_seconds ) );
        if( const std::optional<vpart_reference> openable_part = vp.part_with_feature( "OPENABLE",
                true ); openable_part.has_value() ) {
            const std::string name = openable_part->info().name();
            return string_format( vp->vehicle().part( openable_part->part_index() ).locked ?
                                  _( "That %s is locked." ) : _( "That %s is already open." ), name );
        }
        return _( "There is nothing that can be opened nearby." );
    }
    if( here.open_door( guy, p, !here.is_outside( guy.pos_bub() ) ) ) {
        guy.add_msg_if_player( _( "You open the %s." ), here.name( p ) );
        return std::string();
    }
    if( here.has_flag( ter_furn_flag::TFLAG_LOCKED, p ) ) {
        return _( "The door is locked!" );
    }
    guy.mod_moves( to_moves<int>( 1_seconds ) );
    if( here.ter( p ).obj().close ) {
        return _( "That door is already open." );
    }
    return _( "No door there." );
}

// avatar::smash() and handle_action.cpp smash() for the second player.
std::string smash( npc &guy, tripoint_bub_ms smashp )
{
    map &here = get_map();
    const int move_cost = !guy.is_armed() ? 80 : guy.get_wielded_item()->attack_time( guy ) * 0.8;
    const int smashskill = guy.smash_ability();
    bool smash_floor = false;
    if( smashp.z() != guy.posz() ) {
        if( smashp.z() > guy.posz() ) {
            return std::string();
        }
        smashp.z() = guy.posz();
        smash_floor = true;
    }
    get_event_bus().send<event_type::character_smashes_tile>(
        guy.getID(), here.ter( smashp ).id(), here.furn( smashp ).id() );
    for( std::pair<const field_type_id, field_entry> &fd_to_smsh : here.field_at( smashp ) ) {
        const std::optional<map_fd_bash_info> &bash_info = fd_to_smsh.first->bash_info;
        if( !bash_info ) {
            continue;
        }
        if( ( smashskill < bash_info->str_min && one_in( 10 ) ) || fd_to_smsh.first->indestructible ) {
            guy.add_msg_if_player( m_neutral, _( "You don't seem to be damaging the %s." ),
                                   fd_to_smsh.first->get_name() );
        } else if( smashskill >= rng( bash_info->str_min, bash_info->str_max ) ) {
            sounds::sound( smashp, bash_info->sound_vol, sounds::sound_t::combat, bash_info->sound, true,
                           "smash", "field" );
            here.remove_field( smashp, fd_to_smsh.first );
            here.spawn_items( smashp, item_group::items_from( bash_info->drop_group, calendar::turn ) );
            if( !bash_info->destroyed_field.first.is_null() ) {
                here.add_field( smashp, bash_info->destroyed_field.first, bash_info->destroyed_field.second );
            }
            guy.mod_moves( - bash_info->fd_bash_move_cost );
            guy.add_msg_if_player( m_info, bash_info->field_bash_msg_success.translated() );
        } else {
            sounds::sound( smashp, bash_info->sound_fail_vol, sounds::sound_t::combat, bash_info->sound_fail,
                           true, "smash", "field" );
        }
        return std::string();
    }
    for( const item &maybe_corpse : here.i_at( smashp ) ) {
        if( maybe_corpse.can_revive() ) {
            guy.assign_activity( pulp_activity_actor( here.get_abs( smashp ) ) );
            return std::string();
        }
    }
    if( vehicle *veh = veh_pointer_or_null( here.veh_at( smashp ) ) ) {
        if( !veh->handle_potential_theft( guy ) ) {
            return std::string();
        }
    }
    if( !guy.has_weapon() ) {
        const std::pair<bodypart_id, int> best_part_to_smash = guy.best_part_to_smash();
        if( best_part_to_smash.first != bodypart_str_id::NULL_ID() && here.is_bashable( smashp ) ) {
            std::string name_to_bash = _( "thing" );
            if( here.is_bashable_furn( smashp ) ) {
                name_to_bash = here.furnname( smashp );
            } else if( here.is_bashable_ter( smashp ) ) {
                name_to_bash = here.tername( smashp );
            }
            if( !best_part_to_smash.first->smash_message.empty() ) {
                guy.add_msg_if_player( best_part_to_smash.first->smash_message, name_to_bash );
            } else {
                guy.add_msg_if_player( _( "You use your %s to smash the %s." ),
                                       body_part_name_accusative( best_part_to_smash.first ), name_to_bash );
            }
        }
    }
    const bash_params bash_result = here.bash( smashp, smashskill, false, false, smash_floor );
    if( !bash_result.did_bash ) {
        return _( "There's nothing there to smash!" );
    }
    guy.set_activity_level( MODERATE_EXERCISE );
    guy.handle_melee_wear( guy.used_weapon() );
    const float weary_mult = 1.0f / guy.exertion_adjusted_move_multiplier( MODERATE_EXERCISE );
    guy.burn_energy_arms( 2 * guy.get_standard_stamina_cost() );
    if( static_cast<int>( guy.get_skill_level( skill_melee ) ) == 0 ) {
        guy.practice( skill_melee, rng( 0, 1 ) * rng( 0, 1 ) );
    }
    guy.mod_moves( -move_cost * weary_mult );
    guy.recoil = MAX_RECOIL;
    if( bash_result.success ) {
        return std::string();
    }
    const int resistance = here.bash_resistance( smashp );
    if( smashskill >= resistance ) {
        // handle_action.cpp smash(): keep at it.
        if( resistance > 0 && query_yn( _( "Keep smashing until destroyed?" ) ) ) {
            guy.assign_activity( bash_activity_actor( smashp ) );
        }
    } else if( one_in( 10 ) ) {
        guy.add_msg_if_player( m_neutral, _( "You don't seem to be damaging the %s." ),
                               here.has_furn( smashp ) && here.furn( smashp ).obj().bash ?
                               here.furnname( smashp ) : here.tername( smashp ) );
    }
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
    } else if( action == "up" || action == "down" ) {
        // npc::move_to() takes a z-level step as a climb of the stairs.
        map &here = get_map();
        const tripoint_bub_ms from = guy.pos_bub();
        const bool up = action == "up";
        if( !here.has_flag( up ? ter_furn_flag::TFLAG_GOES_UP : ter_furn_flag::TFLAG_GOES_DOWN, from ) ) {
            return up ? _( "You can't go up here!" ) : _( "You can't go down here!" );
        }
        const tripoint_bub_ms to = from + tripoint_rel_ms( 0, 0, up ? 1 : -1 );
        if( !here.inbounds( to ) ) {
            return _( "too far from the host" );
        }
        guy.move_to( to, true );
        return std::string();
    } else if( action == "drive" ) {
        // game::control_vehicle(), for the controls under the character.
        map &here = get_map();
        const optional_vpart_position vp = here.veh_at( guy.pos_bub() );
        if( !vp ) {
            return _( "No vehicle controls found." );
        }
        vehicle &veh = vp->vehicle();
        if( guy.controlling_vehicle ) {
            guy.controlling_vehicle = false;
            add_msg( _( "%1$s lets go of the controls of the %2$s." ), guy.get_name(), veh.name );
            return std::string();
        }
        if( veh.avail_part_with_feature( vp->mount_pos(), "CONTROLS" ) < 0 ) {
            return _( "You can't drive the vehicle from here.  You need controls!" );
        }
        if( !guy.in_vehicle ) {
            return _( "get in the vehicle first" );
        }
        if( veh.is_locked ) {
            return _( "the vehicle is locked" );
        }
        if( veh.engine_on ) {
            guy.controlling_vehicle = true;
            add_msg( _( "%1$s takes control of the %2$s." ), guy.get_name(), veh.name );
        } else {
            veh.start_engines( here, &guy, true );
        }
        return std::string();
    } else if( action == "autoattack" ) {
        // avatar_action::autoattack(): the nearest hostile within reach.
        map &here = get_map();
        Creature *best = nullptr;
        for( Creature &critter : g->all_creatures() ) {
            if( &critter == &guy || critter.is_dead_state() || !guy.sees( here, critter ) ||
                guy.attitude_to( critter ) != Creature::Attitude::HOSTILE ||
                rl_dist( critter.pos_bub(), guy.pos_bub() ) > 1 ) {
                continue;
            }
            if( best == nullptr || critter.get_hp() < best->get_hp() ) {
                best = &critter;
            }
        }
        if( best == nullptr ) {
            return _( "No hostile creature in reach.  Waiting a turn." );
        }
        guy.melee_attack( *best, true );
        return std::string();
    } else if( action == "grab" ) {
        return npc_grab::toggle( guy, dir );
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
        case ACTION_MOVE_UP:
            send_command( "up", tripoint_rel_ms::zero );
            return true;
        case ACTION_MOVE_DOWN:
            send_command( "down", tripoint_rel_ms::zero );
            return true;
        case ACTION_AUTOATTACK:
            send_command( "autoattack", tripoint_rel_ms::zero );
            return true;
        case ACTION_CONTROL_VEHICLE:
            send_command( "drive", tripoint_rel_ms::zero );
            return true;
        case ACTION_GRAB:
            // handle_action.cpp, grab(): a held thing is let go of.
            if( get_avatar().get_grab_type() != object_type::NONE ) {
                send_command( "grab", tripoint_rel_ms::zero );
                return true;
            }
            if( const std::optional<tripoint_bub_ms> p = choose_adjacent( _( "Grab where?" ) ) ) {
                send_command( "grab", *p - get_avatar().pos_bub() );
            } else {
                add_msg( _( "Never mind." ) );
            }
            return true;
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
        // A vehicle: its menu runs on the client's copy (game::examine()),
        // what it starts goes to the host as an activity.
        map &here = get_map();
        const tripoint_bub_ms p = get_avatar().pos_bub() + *dir;
        if( action == "examine" ) {
            if( const optional_vpart_position vp = here.veh_at( p ) ) {
                vp->vehicle().interact_with( &here, p, act == ACTION_EXAMINE_AND_PICKUP );
                return true;
            }
        }
        send_command( action, *dir );
    }
    return true;
}

} // namespace mp::world_actions
