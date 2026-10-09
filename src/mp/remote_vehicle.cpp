#include "mp/remote_vehicle.h"

#include <functional>
#include <sstream>
#include <vector>

#include "action.h"
#include "calendar.h"
#include "character.h"
#include "character_id.h"
#include "game.h"
#include "game_constants.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "map.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/protocol.h"
#include "mp/rc_npc.h"
#include "mp/remote_actions.h"
#include "mp/remote_prompt.h"
#include "npc.h"
#include "output.h"
#include "overmapbuffer.h"
#include "translations.h"
#include "uilist.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

static const itype_id fuel_type_battery( "battery" );
static const itype_id itype_plut_cell( "plut_cell" );

namespace mp::remote_vehicle
{

namespace
{

void send( const std::string &op, const vehicle &veh,
           const std::function<void( JsonOut & )> &write )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", "vehicle_edit" );
    json.member( "op", op );
    json.member( "vehicle", veh.pos_abs() );
    write( json );
    json.end_object();
    net::client_send_line( os.str() );
}

// The vehicle the client means: the one whose origin is at that place.
vehicle *find( const tripoint_abs_ms &origin )
{
    map &here = get_map();
    const tripoint_bub_ms p = here.get_bub( origin );
    if( !here.inbounds( p ) ) {
        return nullptr;
    }
    for( const wrapped_vehicle &v : here.get_vehicles() ) {
        if( v.v->pos_abs() == origin ) {
            return v.v;
        }
    }
    return nullptr;
}

// act_vehicle_unload_fuel() for the second player's character.
void unload_fuel( npc &guy, vehicle &veh )
{
    map &here = get_map();
    std::vector<itype_id> fuels;
    for( auto &e : veh.fuels_left() ) {
        const itype *type = item::find_type( e.first );
        if( e.first == fuel_type_battery || type->phase != phase_id::SOLID ) {
            continue;
        }
        fuels.push_back( e.first );
    }
    if( fuels.empty() ) {
        guy.add_msg_if_player( m_info, _( "The vehicle has no solid fuel left to remove." ) );
        return;
    }
    itype_id fuel = fuels.front();
    if( fuels.size() > 1 ) {
        uilist smenu;
        smenu.text = _( "Remove what?" );
        for( const itype_id &f : fuels ) {
            smenu.addentry( item::nname( f ) );
        }
        smenu.query();
        if( smenu.ret < 0 || static_cast<size_t>( smenu.ret ) >= fuels.size() ) {
            return;
        }
        fuel = fuels[smenu.ret];
    }
    const int qty = veh.fuel_left( here, fuel );
    if( fuel == itype_plut_cell ) {
        if( qty / PLUTONIUM_CHARGES == 0 ) {
            guy.add_msg_if_player( m_info, _( "The vehicle has no charged plutonium cells." ) );
            return;
        }
        item plutonium( fuel, calendar::turn, qty / PLUTONIUM_CHARGES );
        guy.i_add( plutonium );
        veh.drain( here, fuel, qty - ( qty % PLUTONIUM_CHARGES ) );
    } else {
        item solid_fuel( fuel, calendar::turn, qty );
        guy.i_add( solid_fuel );
        veh.drain( here, fuel, qty );
    }
}

} // namespace

void renamed( const vehicle &veh )
{
    if( !remote_actions::client_active() ) {
        return;
    }
    send( "rename", veh, [&]( JsonOut & json ) {
        json.member( "name", veh.name );
    } );
}

void relabeled( const vehicle &veh, const point_rel_ms &mount, const std::string &label )
{
    if( !remote_actions::client_active() ) {
        return;
    }
    send( "label", veh, [&]( JsonOut & json ) {
        json.member( "mount", mount );
        json.member( "label", label );
    } );
}

void crew_changed( const vehicle &veh, const int part, const int npc_id )
{
    if( !remote_actions::client_active() ) {
        return;
    }
    send( "crew", veh, [&]( JsonOut & json ) {
        json.member( "part", part );
        json.member( "npc", npc_id );
    } );
}

void shape_changed( const vehicle &veh, const int part, const std::string &variant )
{
    if( !remote_actions::client_active() ) {
        return;
    }
    send( "shape", veh, [&]( JsonOut & json ) {
        json.member( "part", part );
        json.member( "variant", variant );
    } );
}

bool fuel_unloaded( const vehicle &veh )
{
    if( !remote_actions::client_active() ) {
        return false;
    }
    send( "unload", veh, []( JsonOut & ) {} );
    return true;
}

void reopen( const JsonObject &question )
{
    tripoint_abs_ms origin;
    question.read( "vehicle", origin );
    point_rel_ms int_p;
    question.read( "int", int_p );
    vehicle *veh = find( origin );
    if( veh == nullptr ) {
        return;
    }
    if( veh->is_appliance() ) {
        g->exam_appliance( *veh, int_p );
    } else {
        g->exam_vehicle( *veh, int_p );
    }
}

std::string edit( npc &guy, const JsonObject &request )
{
    tripoint_abs_ms origin;
    request.read( "vehicle", origin );
    vehicle *veh = find( origin );
    if( veh == nullptr ) {
        return _( "the vehicle is gone" );
    }
    if( rl_dist( veh->pos_abs(), guy.pos_abs() ) > 60 ) {
        return _( "the vehicle is too far away" );
    }
    const std::string op = request.get_string( "op", "" );
    if( op == "rename" ) {
        // veh_interact::do_rename()
        const std::string name = request.get_string( "name", "" );
        if( !name.empty() ) {
            veh->name = name;
            if( veh->tracking_on ) {
                overmap_buffer.remove_vehicle( veh );
                overmap_buffer.add_vehicle( veh );
            }
        }
        return std::string();
    }
    if( op == "label" ) {
        point_rel_ms mount;
        request.read( "mount", mount );
        const int part = veh->part_at( mount );
        if( part < 0 ) {
            return _( "There are no parts here to label." );
        }
        vpart_position( *veh, part ).set_label( request.get_string( "label", "" ) );
        return std::string();
    }
    const int part = request.get_int( "part", -1 );
    if( op == "crew" || op == "shape" ) {
        if( part < 0 || part >= veh->part_count() ) {
            return _( "no such part" );
        }
        vehicle_part &pt = veh->part( part );
        if( op == "shape" ) {
            const std::string variant = request.get_string( "variant", "" );
            if( pt.info().variants.count( variant ) > 0 ) {
                pt.variant = variant;
            }
            return std::string();
        }
        const int npc_id = request.get_int( "npc", 0 );
        if( npc_id == 0 ) {
            pt.unset_crew();
        } else if( const npc *who = g->critter_by_id<npc>( character_id( npc_id ) ) ) {
            veh->assign_seat( pt, *who );
        }
        return std::string();
    }
    if( op == "unload" ) {
        unload_fuel( guy, *veh );
        return std::string();
    }
    return "unknown vehicle edit \"" + op + "\"";
}

void work_done( const Character &who, const vehicle &veh, const point_rel_ms &int_p )
{
    if( !is_remote_character( who ) || !net::has_client() ) {
        return;
    }
    // Like a question that needs no answer: the client opens the screen
    // after the new state of the vehicle has arrived.
    if( const npc *guy = who.as_npc() ) {
        protocol::send_state( *guy );
    }
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "type", "prompt" );
    json.member( "id", 0 );
    json.member( "kind", "vehicle" );
    json.member( "vehicle", veh.pos_abs() );
    json.member( "int", int_p );
    json.end_object();
    net::send_line( os.str() );
}

std::optional<std::optional<point_rel_ms>> ask_facing( const std::string &part_name )
{
    if( !remote_prompt::active() ) {
        return std::nullopt;
    }
    const std::optional<tripoint_rel_ms> dir = choose_direction( string_format(
                _( "Which way should the new %s face?" ), part_name ) );
    if( !dir ) {
        return std::optional<point_rel_ms>();
    }
    return std::optional<point_rel_ms>( dir->xy() );
}

} // namespace mp::remote_vehicle
