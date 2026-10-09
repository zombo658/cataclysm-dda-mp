#include "mp/npc_step.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "avatar.h"
#include "bodypart.h"
#include "character.h"
#include "creature_tracker.h"
#include "damage.h"
#include "field.h"
#include "game.h"
#include "iexamine.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "mp/player_talk.h"
#include "mp/remote_actions.h"
#include "mp/remote_trade.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "rng.h"
#include "string_formatter.h"
#include "translations.h"
#include "trap.h"
#include "uilist.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

static const damage_type_id damage_bash( "bash" );
static const damage_type_id damage_cut( "cut" );

static const efftype_id effect_bouldering( "bouldering" );
static const efftype_id effect_harnessed( "harnessed" );
static const efftype_id effect_no_sight( "no_sight" );
static const efftype_id effect_pet( "pet" );
static const efftype_id effect_psi_stunned( "psi_stunned" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_stunned( "stunned" );

static const json_character_flag json_flag_ALL_TERRAIN_NAVIGATION( "ALL_TERRAIN_NAVIGATION" );
static const json_character_flag json_flag_CANNOT_MOVE( "CANNOT_MOVE" );
static const json_character_flag json_flag_LEVITATION( "LEVITATION" );


namespace mp::npc_step
{

std::vector<std::string> dangerous_tile( const npc &guy, const tripoint_bub_ms &dest )
{
    map &here = get_map();
    if( guy.is_blind() ) {
        return {};
    }
    std::vector<std::string> harmful_stuff;
    const field fields_here = here.field_at( guy.pos_bub() );
    const auto veh_here = here.veh_at( guy.pos_bub() ).part_with_feature( "BOARDABLE", true );
    const auto veh_dest = here.veh_at( dest ).part_with_feature( "BOARDABLE", true );
    const bool veh_here_inside = veh_here && veh_here->is_inside();
    const bool veh_dest_inside = veh_dest && veh_dest->is_inside();

    for( const std::pair<const field_type_id, field_entry> &e : here.field_at( dest ) ) {
        if( !guy.is_dangerous_field( e.second ) ) {
            continue;
        }
        const bool has_field_here = fields_here.find_field( e.first ) != nullptr;
        const bool empty_effects = e.second.field_effects().empty();
        bool danger_dest = empty_effects;
        bool danger_here = has_field_here && empty_effects;
        for( const field_effect &fe : e.second.field_effects() ) {
            if( !danger_dest ) {
                danger_dest = !( ( fe.immune_in_vehicle && veh_dest ) ||
                                 ( fe.immune_inside_vehicle && veh_dest_inside ) ||
                                 ( fe.immune_outside_vehicle && !veh_dest_inside ) ||
                                 guy.is_immune_effect( fe.id ) || guy.check_immunity_data( fe.immunity_data ) );
            }
            if( has_field_here && !danger_here ) {
                danger_here = !( ( fe.immune_in_vehicle && veh_here ) ||
                                 ( fe.immune_inside_vehicle && veh_here_inside ) ||
                                 ( fe.immune_outside_vehicle && !veh_here_inside ) ||
                                 guy.is_immune_effect( fe.id ) || guy.check_immunity_data( fe.immunity_data ) );
            }
        }
        if( !danger_dest || danger_here ) {
            continue;
        }
        harmful_stuff.push_back( e.second.name() );
    }

    if( here.is_open_air( dest ) && !veh_dest && !guy.has_effect_with_flag( json_flag_LEVITATION ) ) {
        harmful_stuff.emplace_back( "ledge" );
    }

    const trap &tr = here.tr_at( dest );
    if( tr.can_see( dest, guy ) && !tr.is_benign() && !veh_dest ) {
        harmful_stuff.push_back( tr.name() );
    }

    static const std::set< bodypart_str_id > sharp_bps = {
        body_part_eyes, body_part_mouth, body_part_head,
        body_part_leg_l, body_part_leg_r, body_part_foot_l,
        body_part_foot_r, body_part_arm_l, body_part_arm_r,
        body_part_hand_l, body_part_hand_r, body_part_torso
    };
    const auto sharp_bp_check = [&guy]( const bodypart_id & bp ) {
        return guy.immune_to( bp, { damage_cut, 10 } );
    };
    if( here.has_flag( ter_furn_flag::TFLAG_ROUGH, dest ) &&
        !here.has_flag( ter_furn_flag::TFLAG_ROUGH, guy.pos_bub() ) &&
        !guy.has_flag( json_flag_ALL_TERRAIN_NAVIGATION ) && !veh_dest &&
        ( guy.get_armor_type( damage_bash, bodypart_id( "foot_l" ) ) < 5 ||
          guy.get_armor_type( damage_bash, bodypart_id( "foot_r" ) ) < 5 ) ) {
        harmful_stuff.emplace_back( here.name( dest ) );
    } else if( here.has_flag( ter_furn_flag::TFLAG_SHARP, dest ) &&
               !here.has_flag( ter_furn_flag::TFLAG_SHARP, guy.pos_bub() ) &&
               !guy.has_flag( json_flag_ALL_TERRAIN_NAVIGATION ) &&
               !( guy.in_vehicle || here.veh_at( dest ) ) && guy.get_dex() < 78 &&
               !std::all_of( sharp_bps.begin(), sharp_bps.end(), sharp_bp_check ) ) {
        harmful_stuff.push_back( here.name( dest ) );
    }
    return harmful_stuff;
}

namespace
{

// game::prompt_dangerous_tile(): asked of the second player.
bool confirm_dangerous( const std::vector<std::string> &harmful )
{
    return harmful.empty() ||
           query_yn( _( "Really step into %s?" ), enumerate_as_string( harmful ) );
}

// The danger check of game::walk_move(); true if the step goes on.
bool dangerous_step_allowed( npc &guy, const tripoint_bub_ms &dest )
{
    const std::vector<std::string> harmful = dangerous_tile( guy, dest );
    if( harmful.empty() ) {
        return true;
    }
    if( harmful.size() == 1 && harmful[0] == "ledge" ) {
        iexamine::ledge( guy, dest );
        return false;
    }
    const std::string prompt = get_option<std::string>( "DANGEROUS_TERRAIN_WARNING_PROMPT" );
    if( prompt == "ALWAYS" ) {
        return confirm_dangerous( harmful );
    }
    if( prompt == "RUNNING" && ( !guy.is_running() || !confirm_dangerous( harmful ) ) ) {
        guy.add_msg_if_player( m_warning,
                               _( "Stepping into that %1$s looks risky.  Run into it if you wish to enter anyway." ),
                               enumerate_as_string( harmful ) );
        return false;
    }
    if( prompt == "CROUCHING" && ( !guy.is_crouching() || !confirm_dangerous( harmful ) ) ) {
        guy.add_msg_if_player( m_warning,
                               _( "Stepping into that %1$s looks risky.  Crouch and move into it if you wish to enter anyway." ),
                               enumerate_as_string( harmful ) );
        return false;
    }
    if( prompt == "NEVER" && !guy.is_running() ) {
        guy.add_msg_if_player( m_warning,
                               _( "Stepping into that %1$s looks risky.  Run into it if you wish to enter anyway." ),
                               enumerate_as_string( harmful ) );
        return false;
    }
    return true;
}

// The end of a step, as npc::move_to() does it.
void arrive( npc &guy, const tripoint_bub_ms &old_pos, const tripoint_bub_ms &p )
{
    map &here = get_map();
    guy.make_footstep_noise();
    guy.setpos( here, p );
    if( guy.is_mounted() && guy.mounted_creature->pos_abs() != guy.pos_abs() ) {
        guy.mounted_creature->setpos( guy.pos_abs() );
        guy.mounted_creature->process_triggers();
        here.creature_in_field( *guy.mounted_creature );
        here.creature_on_trap( *guy.mounted_creature );
    }
    if( here.has_flag( ter_furn_flag::TFLAG_UNSTABLE, p ) && !here.has_vehicle_floor( p ) ) {
        guy.add_effect( effect_bouldering, 1_turns, true );
    } else if( guy.has_effect( effect_bouldering ) ) {
        guy.remove_effect( effect_bouldering );
    }
    if( here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_NO_SIGHT, p ) ) {
        guy.add_effect( effect_no_sight, 1_turns, true );
    } else if( guy.has_effect( effect_no_sight ) ) {
        guy.remove_effect( effect_no_sight );
    }
    if( guy.in_vehicle ) {
        here.unboard_vehicle( old_pos );
    }
    if( here.veh_at( p ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        here.board_vehicle( p, &guy );
    }
    here.creature_on_trap( guy );
    here.creature_in_field( guy );
}

// game::npc_menu(), what of it the second player can do.
void npc_menu( npc &guy, npc &who )
{
    enum choices : int { talk = 0, swap_pos, push, attack, trade };
    const bool obeys = who.is_player_ally() && !who.in_sleep_state();
    uilist amenu;
    amenu.text = string_format( _( "What to do with %s?" ), who.disp_name() );
    amenu.addentry( talk, true, 't', _( "Talk" ) );
    amenu.addentry( swap_pos, obeys && !who.is_mounted() && !guy.is_mounted(), 's',
                    _( "Swap positions" ) );
    amenu.addentry( push, !who.is_enemy() && !who.in_sleep_state() && !who.is_mounted(), 'p',
                    _( "Push away" ) );
    amenu.addentry( attack, true, 'a', _( "Attack" ) );
    if( who.is_player_ally() ) {
        amenu.addentry( trade, true, 'b', _( "Trade" ) );
    }
    amenu.query();
    switch( amenu.ret ) {
        case talk: {
            const std::string why_not = talk_hooks::talk( guy, who );
            if( !why_not.empty() ) {
                guy.add_msg_if_player( m_info, why_not );
            }
            break;
        }
        case swap_pos:
            if( !confirm_dangerous( dangerous_tile( guy, who.pos_bub() ) ) ) {
                break;
            }
            guy.add_msg_if_player( _( "You swap places with %s." ), who.get_name() );
            g->swap_critters( guy, who );
            guy.mod_moves( -200 );
            break;
        case push: {
            const tripoint_bub_ms oldpos = who.pos_bub();
            who.move_away_from( guy.pos_bub(), true );
            guy.mod_moves( -20 );
            if( oldpos != who.pos_bub() ) {
                guy.add_msg_if_player( _( "%s moves out of the way." ), who.get_name() );
            } else {
                guy.add_msg_if_player( m_warning, _( "%s has nowhere to go!" ), who.get_name() );
            }
            break;
        }
        case trade:
            remote_trade::trade_with_npc( guy, who, 0, _( "Trade" ) );
            break;
        case attack:
            if( query_yn( _( "You may be attacked!  Proceed?" ) ) ) {
                guy.melee_attack( who, true );
                who.make_angry();
            }
            break;
        default:
            break;
    }
}

} // namespace

bool step( npc &guy, const tripoint_rel_ms &d )
{
    map &here = get_map();
    const tripoint_bub_ms pos = guy.pos_bub( here );
    const tripoint_bub_ms dest = pos + d;
    if( d.z() != 0 || guy.has_effect( effect_stunned ) || guy.has_effect( effect_psi_stunned ) ||
        here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, dest ) ||
        here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, dest ) ) {
        return false;
    }
    creature_tracker &creatures = get_creature_tracker();
    if( !guy.move_effects( creatures.creature_at( dest ) != nullptr ) ) {
        guy.set_moves( 0 );
        return true;
    }

    // Creatures in the way: avatar_action::move().
    if( monster *const mon = creatures.creature_at<monster>( dest, true ) ) {
        if( mon->friendly == 0 && !mon->has_effect( effect_pet ) ) {
            if( mon->attitude_to( guy ) == Creature::Attitude::NEUTRAL &&
                !query_yn( _( "You may be attacked!  Proceed?" ) ) ) {
                return true;
            }
            guy.melee_attack( *mon, true );
            if( mon->is_hallucination() ) {
                mon->die( &here, &guy );
            }
            return true;
        }
        if( mon->has_flag( mon_flag_IMMOBILE ) || mon->has_effect( effect_harnessed ) ||
            mon->has_effect( effect_ridden ) ) {
            guy.add_msg_if_player( m_info, _( "You can't displace your %s." ), mon->name() );
            return true;
        }
        // game::walk_move(): a pet trades places.
        if( !dangerous_step_allowed( guy, dest ) ) {
            return true;
        }
        guy.add_msg_if_player( _( "You push the %s out of the way." ), mon->name() );
        g->swap_critters( guy, *mon );
        guy.mod_moves( -guy.run_cost( here.combined_movecost( dest, pos ), d.x() != 0 && d.y() != 0 ) );
        return true;
    }
    if( npc *const who = creatures.creature_at<npc>( dest ) ) {
        if( who->is_enemy() ) {
            guy.melee_attack( *who, true );
            who->make_angry();
        } else {
            npc_menu( guy, *who );
        }
        return true;
    }
    if( creatures.creature_at( dest ) == &get_avatar() ) {
        // The host: trade places or things, talk (mp/player_talk.h).
        player_talk::host_menu( guy );
        return true;
    }
    if( guy.has_flag( json_flag_CANNOT_MOVE ) ) {
        guy.add_msg_if_player( m_bad, _( "You cannot move!" ) );
        return true;
    }

    // Doors: walked through when open, opened when walking into them.
    const optional_vpart_position vp0 = here.veh_at( pos );
    const optional_vpart_position vp1 = here.veh_at( dest );
    vehicle *const veh0 = veh_pointer_or_null( vp0 );
    vehicle *const veh1 = veh_pointer_or_null( vp1 );
    const bool outside_vehicle = veh0 == nullptr || veh0 != veh1;
    int dpart = -1;
    bool veh_closed_door = false;
    if( veh1 != nullptr ) {
        dpart = veh1->next_part_to_open( vp1->part_index(), outside_vehicle );
        veh_closed_door = dpart >= 0 && !veh1->part( dpart ).open;
    }
    const std::string door_name = here.obstacle_name( dest );
    if( here.passable_ter_furn( dest ) && guy.is_walking() && !veh_closed_door &&
        here.open_door( guy, dest, !here.is_outside( pos ) ) ) {
        guy.mod_moves( -guy.get_speed() );
        guy.add_msg_if_player( _( "You open the %s." ), door_name );
        return true;
    }

    // game::walk_move(), the plain step.
    if( here.passable_through( dest ) && !veh_closed_door ) {
        if( here.has_flag_ter( ter_furn_flag::TFLAG_SMALL_PASSAGE, dest ) &&
            guy.get_size() > creature_size::medium ) {
            guy.add_msg_if_player( m_warning, _( "You can't fit there." ) );
            return true;
        }
        if( vp1 && !veh1->handle_potential_theft( guy ) ) {
            return true;
        }
        if( !dangerous_step_allowed( guy, dest ) ) {
            return true;
        }
        const bool diag = trigdist && d.x() != 0 && d.y() != 0;
        const int mcost_from = here.move_cost( pos );
        const int cost = guy.run_cost( here.combined_movecost( pos, dest ), diag );
        guy.mod_moves( -cost );
        guy.burn_move_stamina( cost );
        arrive( guy, pos, dest );
        const int mcost_to = here.move_cost( dest );
        if( mcost_to > 100 && mcost_from <= 100 ) {
            guy.add_msg_if_player( m_warning, _( "Moving onto this %s is slow!" ), here.name( dest ) );
        }
        return true;
    }

    if( veh_closed_door ) {
        if( veh1->handle_potential_theft( guy ) ) {
            if( outside_vehicle ) {
                veh1->open_all_at( here, dpart );
            } else {
                veh1->open( here, dpart );
            }
            guy.add_msg_if_player( _( "You open the %1$s's %2$s." ), veh1->name,
                                   veh1->part( dpart ).name() );
            guy.mod_moves( -guy.get_speed() );
        }
        return true;
    }
    if( here.open_door( guy, dest, !here.is_outside( pos ) ) ) {
        guy.mod_moves( -guy.get_speed() );
        guy.add_msg_if_player( _( "You open the %s." ), door_name );
        return true;
    }
    // The way is blocked: as for the host, nothing happens.
    if( here.has_flag( ter_furn_flag::TFLAG_LOCKED, dest ) ) {
        guy.add_msg_if_player( _( "That door is locked!" ) );
    } else if( guy.is_blind() ) {
        guy.add_msg_if_player( _( "You bump into the %s!" ), here.obstacle_name( dest ) );
        guy.mod_moves( -guy.get_speed() );
    }
    return true;
}

} // namespace mp::npc_step
