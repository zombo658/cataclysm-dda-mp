#include "mp/npc_grab.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <list>
#include <map>
#include <optional>
#include <vector>

#include "calendar.h"
#include "character_id.h"
#include "creature_tracker.h"
#include "field_type.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "output.h"
#include "rng.h"
#include "sounds.h"
#include "string_formatter.h"
#include "tileray.h"
#include "translations.h"
#include "trap.h"
#include "units.h"
#include "units_utility.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"

static const field_type_str_id field_fd_fire( "fd_fire" );

static const trait_id trait_CENOBITE( "CENOBITE" );
static const trait_id trait_MASOCHIST( "MASOCHIST" );
static const trait_id trait_MASOCHIST_MED( "MASOCHIST_MED" );

namespace mp::npc_grab
{

namespace
{

std::map<character_id, object_type> grab_types;

void set( npc &guy, const object_type what, const tripoint_rel_ms &point = tripoint_rel_ms::zero )
{
    if( what == object_type::NONE ) {
        grab_types.erase( guy.getID() );
    } else {
        grab_types[guy.getID()] = what;
    }
    guy.grab_point = point;
}

// game::can_move_furniture()
bool can_move_furniture( const npc &guy, const tripoint_bub_ms &fdest, const tripoint_rel_ms &dp )
{
    map &here = get_map();
    const bool pulling_furniture = dp.xy() == -guy.grab_point.xy();
    const bool has_floor = here.has_floor_or_water( fdest );
    creature_tracker &creatures = get_creature_tracker();
    const bool is_ramp_or_road = here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, fdest ) ||
                                 here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, fdest ) ||
                                 here.has_flag( ter_furn_flag::TFLAG_ROAD, fdest );
    if( !here.passable( fdest ) ) {
        return false;
    }
    // The character itself is an npc; the host's check never sees the avatar.
    const npc *other = creatures.creature_at<npc>( fdest );
    if( ( other != nullptr && other != &guy ) ||
        creatures.creature_at<monster>( fdest ) != nullptr ) {
        return false;
    }
    if( !( !pulling_furniture || g->is_empty( guy.pos_bub() + dp ) ) &&
        ( !has_floor || here.has_flag( ter_furn_flag::TFLAG_FLAT, fdest ) || is_ramp_or_road ) ) {
        return false;
    }
    if( here.has_furn( fdest ) ) {
        return false;
    }
    if( here.veh_at( fdest ) ) {
        // Loading furniture onto a vehicle is not done for the second
        // player yet (see furn_move()).
        return false;
    }
    return has_floor ? here.tr_at( fdest ).is_null() : true;
}

// game::grabbed_furn_move()
bool furn_move( npc &guy, const tripoint_rel_ms &dp )
{
    map &here = get_map();
    const tripoint_bub_ms fpos = guy.pos_bub() + guy.grab_point;
    if( !here.has_furn( fpos ) ) {
        guy.add_msg_if_player( m_info, _( "No furniture at grabbed point." ) );
        set( guy, object_type::NONE );
        return false;
    }

    int ramp_z_offset = 0;
    if( here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, fpos + dp.xy() ) ) {
        ramp_z_offset = 1;
    } else if( here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, fpos + dp.xy() ) ) {
        ramp_z_offset = -1;
    }

    const bool pushing_furniture = dp.xy() == guy.grab_point.xy();
    const bool pulling_furniture = dp.xy() == -guy.grab_point.xy();
    const bool shifting_furniture = !pushing_furniture && !pulling_furniture;

    const tripoint_bub_ms fdest = fpos + tripoint_rel_ms( dp.xy(), ramp_z_offset );
    const bool canmove = can_move_furniture( guy, fdest, dp );

    const furn_t furntype = here.furn( fpos ).obj();
    const int src_items = here.i_at( fpos ).size();
    const map_stack &dst_ms = here.i_at( fdest );
    const int dst_items = dst_ms.size();

    const bool only_liquid_items = std::all_of( dst_ms.begin(), dst_ms.end(),
    [&]( const item & liquid_item ) {
        return liquid_item.made_of_from_type( phase_id::LIQUID );
    } );

    const bool dst_item_ok = !here.has_flag( ter_furn_flag::TFLAG_NOITEM, fdest ) &&
                             !here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, fdest ) &&
                             !here.has_flag( ter_furn_flag::TFLAG_DESTROY_ITEM, fdest );
    const bool src_item_ok = furntype.has_flag( ter_furn_flag::TFLAG_CONTAINER ) ||
                             furntype.has_flag( ter_furn_flag::TFLAG_FIRE_CONTAINER ) ||
                             furntype.has_flag( ter_furn_flag::TFLAG_SEALED );

    const int fire_intensity = here.get_field_intensity( fpos, field_fd_fire );
    const time_duration fire_age = here.get_field_age( fpos, field_fd_fire );

    int str_req = furntype.move_str_req;
    units::mass furniture_contents_weight = 0_gram;
    for( item &contained_item : here.i_at( fpos ) ) {
        furniture_contents_weight += contained_item.weight();
    }
    str_req += furniture_contents_weight / 4_kilogram;
    const int str = guy.get_arm_str();

    if( !canmove ) {
        guy.add_msg_if_player( _( "The %s collides with something." ), furntype.name() );
        return true;
    } else if( str_req > str && guy.get_perceived_pain() > 40 &&
               !guy.has_trait( trait_CENOBITE ) && !guy.has_trait( trait_MASOCHIST ) &&
               !guy.has_trait( trait_MASOCHIST_MED ) ) {
        guy.add_msg_if_player( m_bad, _( "You are in too much pain to try moving the heavy %s!" ),
                               furntype.name() );
        return true;
    } else if( str_req > str && guy.get_perceived_pain() > 50 &&
               ( guy.has_trait( trait_MASOCHIST ) || guy.has_trait( trait_MASOCHIST_MED ) ) ) {
        guy.add_msg_if_player( m_bad,
                               _( "Even with your appetite for pain, you are in too much pain to try moving the heavy %s!" ),
                               furntype.name() );
        return true;
    } else if( str_req > str && one_in( std::max( 20 - str_req - str, 2 ) ) ) {
        guy.add_msg_if_player( m_bad, _( "You strain yourself trying to move the heavy %s!" ),
                               furntype.name() );
        guy.mod_pain( 1 );
        return true;
    } else if( !src_item_ok && !only_liquid_items && dst_items > 0 ) {
        guy.add_msg_if_player( _( "There's stuff in the way." ) );
        return true;
    }

    if( str_req > str ) {
        const int move_penalty = std::pow( str_req, 2.0 ) + 100.0;
        if( move_penalty <= 1000 ) {
            if( str >= str_req - 3 ) {
                guy.add_msg_if_player( m_bad, _( "The %s is really heavy!" ), furntype.name() );
                if( one_in( 3 ) ) {
                    guy.add_msg_if_player( m_bad, _( "You fail to move the %s." ), furntype.name() );
                    return true;
                }
            } else {
                guy.add_msg_if_player( m_bad, _( "The %s is too heavy for you to budge." ), furntype.name() );
                return true;
            }
        }
        guy.mod_moves( -move_penalty );
    }
    sounds::sound( fdest, furntype.move_str_req * 2, sounds::sound_t::movement,
                   _( "a scraping noise." ), true, "misc", "scraping" );

    here.furn_set( fdest, here.furn( fpos ), false, false, true );
    here.furn_set( fpos, furn_str_id::NULL_ID(), true );

    if( fire_intensity == 1 && !pulling_furniture ) {
        here.remove_field( fpos, field_fd_fire );
        here.set_field_intensity( fdest, field_fd_fire, fire_intensity );
        here.set_field_age( fdest, field_fd_fire, fire_age );
    }

    if( dst_items > 0 && only_liquid_items ) {
        here.i_clear( fdest );
    }

    if( src_items > 0 ) {
        if( dst_item_ok && src_item_ok ) {
            std::list<item> temp;
            map_stack ms = here.i_at( fpos );
            std::move( ms.begin(), ms.end(), std::back_inserter( temp ) );
            here.i_clear( fpos );
            here.i_clear( fdest );
            for( item &cur_item : temp ) {
                here.add_item_or_charges( fdest, cur_item );
            }
        } else {
            guy.add_msg_if_player( _( "Stuff spills from the %s!" ), furntype.name() );
        }
    }

    if( !here.has_floor_or_water( fdest ) && !here.has_flag( ter_furn_flag::TFLAG_FLAT, fdest ) ) {
        guy.add_msg_if_player( _( "You let go of the %1$s as it falls down the %2$s." ), furntype.name(),
                               enumerate_as_string( g->get_dangerous_tile( fdest ) ) );
        set( guy, object_type::NONE );
        return true;
    }

    guy.grab_point.z() += ramp_z_offset;

    if( shifting_furniture ) {
        const tripoint_rel_ms d_sum = guy.grab_point + dp;
        if( std::abs( d_sum.x() ) < 2 && std::abs( d_sum.y() ) < 2 ) {
            guy.grab_point = d_sum;
        } else {
            guy.add_msg_if_player( _( "You let go of the %s." ), furntype.name() );
            set( guy, object_type::NONE );
        }
        return true;
    }

    if( pushing_furniture && here.impassable( fpos ) ) {
        guy.add_msg_if_player( _( "You let go of the %1$s as it slides past %2$s." ),
                               furntype.name(), here.tername( fdest ) );
        set( guy, object_type::NONE );
        return true;
    }
    return false;
}

// game::grabbed_veh_move()
bool veh_move( npc &guy, const tripoint_rel_ms &dp )
{
    map &here = get_map();

    const optional_vpart_position grabbed_vehicle_vp = here.veh_at( guy.pos_bub( here ) +
            guy.grab_point );
    if( !grabbed_vehicle_vp ) {
        guy.add_msg_if_player( m_info, _( "No vehicle at grabbed point." ) );
        set( guy, object_type::NONE );
        return false;
    }
    vehicle *grabbed_vehicle = &grabbed_vehicle_vp->vehicle();
    if( !grabbed_vehicle->handle_potential_theft( guy ) ) {
        return false;
    }
    const int grabbed_part = grabbed_vehicle_vp->part_index();
    if( monster *mon = grabbed_vehicle->get_harnessed_animal( here ) ) {
        guy.add_msg_if_player( m_info, _( "You cannot move this vehicle whilst your %s is harnessed!" ),
                               mon->get_name() );
        set( guy, object_type::NONE );
        return false;
    }
    const vehicle *veh_under_player = veh_pointer_or_null( here.veh_at( guy.pos_bub( here ) ) );
    if( grabbed_vehicle == veh_under_player ) {
        guy.grab_point = - dp;
        return false;
    }

    tripoint_rel_ms dp_veh = - guy.grab_point;
    const tripoint_rel_ms prev_grab = guy.grab_point;
    tripoint_rel_ms next_grab = guy.grab_point;
    const tileray initial_veh_face = grabbed_vehicle->face;

    const bool veh_has_solid = !empty( grabbed_vehicle->get_avail_parts( VPFLAG_OBSTACLE ) );
    const bool veh_single_tile = grabbed_vehicle->get_points().size() == 1;
    bool zigzag = false;
    bool pushing = false;
    bool pulling = false;

    if( dp == prev_grab ) {
        dp_veh = dp;
        pushing = true;
    } else if( std::abs( dp.x() + dp_veh.x() ) != 2 && std::abs( dp.y() + dp_veh.y() ) != 2 ) {
        guy.grab_point = - ( dp + dp_veh );
        return false;
    } else if( ( dp.x() == prev_grab.x() || dp.y() == prev_grab.y() ) &&
               next_grab.x() != 0 && next_grab.y() != 0 ) {
        dp_veh.x() = dp.x() == -dp_veh.x() ? 0 : dp_veh.x();
        dp_veh.y() = dp.y() == -dp_veh.y() ? 0 : dp_veh.y();
        next_grab = - dp_veh;
        zigzag = true;
    } else {
        next_grab = - dp;
        pulling = true;
    }

    grabbed_vehicle->invalidate_mass();

    int mc = 0;
    const int max_str_req = grabbed_vehicle->total_mass( here ) / 10_kilogram;
    int str_req = 0;
    const int str = guy.get_arm_str();

    bool bad_veh_angle = false;
    bool invalid_veh_turndir = false;
    bool invalid_veh_face = false;
    bool back_of_vehicle = false;

    const auto &wheel_indices = grabbed_vehicle->wheelcache;
    if( grabbed_vehicle->valid_wheel_config( here ) ) {
        if( veh_has_solid && !veh_single_tile && grabbed_vehicle->steering_effectiveness( here ) > 0 ) {
            tileray my_dir;
            my_dir.init( dp.xy() );
            const units::angle face_delta = angle_delta( grabbed_vehicle->face.dir(), my_dir.dir() );

            tileray my_pos_dir;
            const tripoint_rel_ms my_angle = guy.pos_bub( here ) - grabbed_vehicle->pos_bub( here );
            my_pos_dir.init( my_angle.xy() );
            back_of_vehicle = angle_delta( grabbed_vehicle->face.dir(), my_pos_dir.dir() ) > 90_degrees;
            invalid_veh_face = face_delta > vehicles::steer_increment * 2 - 1_degrees &&
                               face_delta < 180_degrees - vehicles::steer_increment * 2 + 1_degrees;
            invalid_veh_turndir = normalize( angle_delta( grabbed_vehicle->turn_dir,
                                             grabbed_vehicle->face.dir() ), 180_degrees ) > vehicles::steer_increment * 4 - 1_degrees;
            bad_veh_angle = invalid_veh_face || invalid_veh_turndir;
            if( bad_veh_angle ) {
                str_req = max_str_req;
            }
        } else {
            str_req = max_str_req / 10;
            const tripoint_bub_ms vehpos = grabbed_vehicle->pos_bub( here );
            for( int p : wheel_indices ) {
                const tripoint_bub_ms wheel_pos = vehpos + grabbed_vehicle->part( p ).precalc[0];
                const int mapcost = here.move_cost( wheel_pos, grabbed_vehicle );
                mc += str_req * mapcost / wheel_indices.size();
            }
            if( wheel_indices.size() > 4 || wheel_indices.size() == 1 ) {
                str_req = mc / 4 + 1;
            } else {
                str_req = mc / wheel_indices.size() + 1;
            }
            str_req /= grabbed_vehicle->k_traction( here, here.vehicle_wheel_traction( *grabbed_vehicle ) );
            str_req = std::min( str_req, max_str_req );
        }
    } else {
        str_req = max_str_req;
    }

    if( str_req <= str ) {
        if( str_req == max_str_req ) {
            sounds::sound( grabbed_vehicle->pos_bub( here ), str_req * 2, sounds::sound_t::movement,
                           _( "a scraping noise." ), true, "misc", "scraping" );
        }
        guy.mod_moves( -to_moves<int>( 4_seconds ) * str_req / std::max( 1, str ) );
        guy.burn_energy_all( -200 * str_req / std::max( 1, str ) );
        const int ex = dice( 1, 6 ) - 1 + str_req;
        if( ex > str + 1 ) {
            guy.add_msg_if_player( m_bad, _( "You strain yourself to move the %s!" ), grabbed_vehicle->name );
            guy.mod_moves( -to_moves<int>( 2_seconds ) );
            guy.mod_pain( 1 );
        } else if( ex >= str ) {
            guy.add_msg_if_player( _( "It takes some time to move the %s." ), grabbed_vehicle->name );
            guy.mod_moves( -to_moves<int>( 2_seconds ) );
        }
    } else {
        if( invalid_veh_face ) {
            guy.add_msg_if_player( m_bad, _( "The %s is at too sharp an angle to move like this!" ),
                                   grabbed_vehicle->name );
        }
        if( invalid_veh_turndir ) {
            guy.add_msg_if_player( m_bad, _( "The %s is steered too far to move like this!" ),
                                   grabbed_vehicle->name );
        }
        if( !bad_veh_angle ) {
            guy.add_msg_if_player( m_bad, _( "You lack the strength to move the %s." ),
                                   grabbed_vehicle->name );
        }
        guy.mod_moves( -to_moves<int>( 1_seconds ) );
        return true;
    }

    std::string blocker_name = _( "errors in movement code" );
    const auto get_move_dir = [&]( const tripoint_rel_ms & md_dp_veh,
    const tripoint_rel_ms & md_next_grab ) {
        tileray mdir;
        mdir.init( md_dp_veh.xy() );
        const units::angle turn = normalize( mdir.dir() - grabbed_vehicle->face.dir() );
        if( grabbed_vehicle->is_on_ramp && turn == 180_degrees ) {
            guy.add_msg_if_player( m_bad, _( "The %s can't be turned around while on a ramp." ),
                                   grabbed_vehicle->name );
            return tripoint_rel_ms::zero;
        }
        units::angle precalc_dir = grabbed_vehicle->face.dir();
        if( veh_has_solid && !bad_veh_angle && !veh_single_tile ) {
            const units::angle abs_turn_delta = angle_delta( grabbed_vehicle->face.dir(),
                                                grabbed_vehicle->turn_dir );
            if( abs_turn_delta != 0_degrees ) {
                const int clockwise = std::abs( normalize( grabbed_vehicle->face.dir() +
                                                abs_turn_delta ).value() - normalize( grabbed_vehicle->turn_dir ).value() ) < 0.1 ? 1 : -1;
                units::angle turn_delta = abs_turn_delta * clockwise;
                if( ( pushing && !back_of_vehicle ) || ( pulling && back_of_vehicle ) ) {
                    turn_delta *= -1;
                }
                grabbed_vehicle->turn_dir = normalize( grabbed_vehicle->face.dir() + turn_delta );
                grabbed_vehicle->face = tileray( grabbed_vehicle->turn_dir );
                precalc_dir = grabbed_vehicle->face.dir();
            }
        } else if( !veh_has_solid || veh_single_tile ) {
            grabbed_vehicle->turn( turn );
            grabbed_vehicle->face = tileray( grabbed_vehicle->turn_dir );
            precalc_dir = mdir.dir();
        }
        grabbed_vehicle->precalc_mounts( 1, precalc_dir, grabbed_vehicle->pivot_point( here ) );
        grabbed_vehicle->pos -= grabbed_vehicle->pivot_displacement().raw();

        const tripoint_bub_ms new_part_pos = grabbed_vehicle->pos_bub( here ) +
                                             grabbed_vehicle->part( grabbed_part ).precalc[1];
        const tripoint_bub_ms expected_pos = guy.pos_bub( here ) + dp + md_next_grab;
        tripoint_rel_ms actual_dir = tripoint_rel_ms( ( expected_pos - new_part_pos ).xy(), 0 );

        bool actual_diff = false;
        const tripoint_rel_ms skip = pushing ? guy.grab_point : dp;
        if( veh_has_solid ) {
            bool no_player_collision = false;
            while( !no_player_collision ) {
                no_player_collision = true;
                for( const vpart_reference &vp : grabbed_vehicle->get_all_parts() ) {
                    if( grabbed_vehicle->pos_bub( here ) +
                        vp.part().precalc[1] + actual_dir == guy.pos_bub( here ) + skip ) {
                        no_player_collision = false;
                        break;
                    }
                }
                if( !no_player_collision ) {
                    actual_dir += guy.grab_point;
                    actual_diff = true;
                }
            }
            if( actual_diff ) {
                guy.add_msg_if_player( _( "You let go of the %s as it turns." ), grabbed_vehicle->disp_name() );
                set( guy, object_type::NONE );
            }
        }
        // Move the character out of the way so it can't collide with the vehicle.
        const tripoint_abs_ms player_prev = guy.pos_abs();
        guy.setpos( here, tripoint_bub_ms::zero, false );
        std::vector<veh_collision> colls;
        const bool failed = grabbed_vehicle->collision( here, colls, actual_dir, true );
        guy.setpos( player_prev, false );
        if( !colls.empty() ) {
            blocker_name = colls.front().target_name;
        }
        return failed ? tripoint_rel_ms::invalid : actual_dir;
    };

    tripoint_rel_ms final_dp_veh = get_move_dir( dp_veh, next_grab );
    if( final_dp_veh == tripoint_rel_ms::invalid && zigzag ) {
        final_dp_veh = get_move_dir( - prev_grab, - dp );
        next_grab = - dp;
    }

    if( final_dp_veh == tripoint_rel_ms::invalid ) {
        guy.add_msg_if_player( _( "The %s collides with %s." ), grabbed_vehicle->name, blocker_name );
        guy.grab_point = prev_grab;
        grabbed_vehicle->face = initial_veh_face;
        return true;
    }

    if( guy.grab_point != tripoint_rel_ms::zero ) {
        guy.grab_point = next_grab;
    }

    here.displace_vehicle( *grabbed_vehicle, final_dp_veh );
    here.rebuild_vehicle_level_caches();

    here.level_vehicle( *grabbed_vehicle );
    grabbed_vehicle->check_falling_or_floating();
    if( grabbed_vehicle->is_falling ) {
        guy.add_msg_if_player( _( "You let go of the %1$s as it starts to fall." ),
                               grabbed_vehicle->disp_name() );
        set( guy, object_type::NONE );
        here.set_seen_cache_dirty( grabbed_vehicle->pos_bub( here ) );
        return true;
    }

    for( int p : wheel_indices ) {
        if( one_in( 2 ) ) {
            vehicle_part &vp_wheel = grabbed_vehicle->part( p );
            const tripoint_bub_ms wheel_p = grabbed_vehicle->bub_part_pos( here, vp_wheel );
            grabbed_vehicle->handle_trap( &here, wheel_p, vp_wheel );
        }
    }
    return false;
}

} // namespace

object_type type( const npc &guy )
{
    const auto it = grab_types.find( guy.getID() );
    return it == grab_types.end() ? object_type::NONE : it->second;
}

// handle_action.cpp, grab()
std::string toggle( npc &guy, const tripoint_rel_ms &dir )
{
    map &here = get_map();
    if( type( guy ) != object_type::NONE ) {
        const tripoint_bub_ms p = guy.pos_bub() + guy.grab_point;
        if( const optional_vpart_position vp = here.veh_at( p ) ) {
            guy.add_msg_if_player( _( "You release the %s." ), vp->vehicle().name );
        } else if( here.has_furn( p ) ) {
            guy.add_msg_if_player( _( "You release the %s." ), here.furnname( p ) );
        }
        set( guy, object_type::NONE );
        return std::string();
    }
    if( dir == tripoint_rel_ms::zero ) {
        guy.add_msg_if_player( _( "You get a hold of yourself." ) );
        set( guy, object_type::NONE );
        return std::string();
    }

    tripoint_bub_ms grabp = guy.pos_bub() + dir;
    const optional_vpart_position vp = here.veh_at( grabp );
    if( !( vp || here.has_furn( grabp ) ) ) {
        if( here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, grabp ) ||
            here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, guy.pos_bub() ) ) {
            grabp.z() += 1;
        } else if( here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, grabp ) ||
                   here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, guy.pos_bub() ) ) {
            grabp.z() -= 1;
        }
    }

    if( const optional_vpart_position vp = here.veh_at( grabp ) ) {
        std::string veh_name = vp->vehicle().name;
        if( !vp->vehicle().handle_potential_theft( guy ) ) {
            return std::string();
        }
        if( vp.part_with_feature( VPFLAG_WALL_MOUNTED, false ) ) {
            return _( "You can't move that, it's attached to the wall." );
        }
        if( vp->vehicle().is_powergrid() && vp->vehicle().part_count() > 1 ) {
            if( !query_yn(
                    _( "That's part of a power grid.  Separate it from the grid so you can move it?" ) ) ) {
                return std::string();
            }
            vp->vehicle().separate_from_grid( &here, vp.value().mount_pos() );
            if( const optional_vpart_position &split_vp = here.veh_at( grabp ) ) {
                veh_name = split_vp->vehicle().name;
            } else {
                return _( "Lost the part to drag after splitting power grid!" );
            }
        }
        // Furniture tied down on a vehicle moves with the vehicle: dragging
        // it off alone is not done for the second player yet.
        const optional_vpart_position vp_boarded = here.veh_at( guy.pos_bub() );
        if( vp_boarded && &vp_boarded->vehicle() == &here.veh_at( grabp )->vehicle() &&
            !empty( vp_boarded->vehicle().get_avail_parts( VPFLAG_OBSTACLE ) ) ) {
            return string_format( _( "You can't move the %s while you're boarding it." ), veh_name );
        }
        set( guy, object_type::VEHICLE, grabp - guy.pos_bub() );
        guy.add_msg_if_player( _( "You grab the %s." ), veh_name );
    } else if( here.has_furn( grabp ) ) {
        if( !here.furn( grabp ).obj().is_movable() ) {
            return string_format( _( "You can not grab the %s." ), here.furnname( grabp ) );
        }
        set( guy, object_type::FURNITURE, grabp - guy.pos_bub() );
        if( !here.can_move_furniture( grabp, &guy ) ) {
            guy.add_msg_if_player( _( "You grab the %s. It feels really heavy." ), here.furnname( grabp ) );
        } else {
            guy.add_msg_if_player( _( "You grab the %s." ), here.furnname( grabp ) );
        }
    } else {
        return _( "There's nothing to grab there!" );
    }
    return std::string();
}

// The grab part of game::walk_move()
void prepare_step( npc &guy, const tripoint_rel_ms &dp, bool &may_enter )
{
    may_enter = false;
    const object_type what = type( guy );
    if( what == object_type::NONE ) {
        return;
    }
    map &here = get_map();
    const tripoint_bub_ms pos = guy.pos_bub();
    const tripoint_bub_ms dest = pos + dp;
    const bool pushing = dp.xy() == guy.grab_point.xy();
    const bool pulling = dp.xy() == -guy.grab_point.xy();
    if( what == object_type::FURNITURE ) {
        if( !here.has_furn( pos + guy.grab_point ) ) {
            guy.add_msg_if_player( m_warning, _( "Can't find grabbed object." ) );
            set( guy, object_type::NONE );
            return;
        }
        may_enter = pushing || ( !pushing && !pulling );
    } else if( what == object_type::VEHICLE ) {
        const optional_vpart_position vp_grab = here.veh_at( pos + guy.grab_point );
        if( !vp_grab ) {
            guy.add_msg_if_player( m_warning, _( "Can't find grabbed object." ) );
            set( guy, object_type::NONE );
            return;
        }
        const optional_vpart_position vp_there = here.veh_at( dest );
        if( vp_there && !pushing && !here.impassable( dest ) &&
            !empty( vp_grab->vehicle().get_avail_parts( VPFLAG_OBSTACLE ) ) &&
            &vp_there->vehicle() == &vp_grab->vehicle() ) {
            guy.add_msg_if_player( m_warning, _( "You move into the %s, releasing it." ),
                                   vp_grab->vehicle().name );
            set( guy, object_type::NONE );
            return;
        }
        may_enter = pushing;
    } else {
        set( guy, object_type::NONE );
    }
}

bool drag( npc &guy, const tripoint_rel_ms &dp )
{
    switch( type( guy ) ) {
        case object_type::VEHICLE:
            return veh_move( guy, dp );
        case object_type::FURNITURE:
            return furn_move( guy, dp );
        default:
            return false;
    }
}

void add_to_json( std::string &data, const npc &guy )
{
    const object_type what = type( guy );
    if( what == object_type::NONE || data.empty() || data[0] != '{' ) {
        return;
    }
    // The names avatar::load() reads.
    const std::string name = what == object_type::VEHICLE ? "OBJECT_VEHICLE" : "OBJECT_FURNITURE";
    data.insert( 1, string_format( "\"grab_point\":[%d,%d,%d],\"grab_type\":\"%s\",",
                                   guy.grab_point.x(), guy.grab_point.y(), guy.grab_point.z(), name ) );
}

} // namespace mp::npc_grab
