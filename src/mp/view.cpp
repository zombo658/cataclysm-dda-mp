#include "mp/view.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "catacharset.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "item.h"
#include "json.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace mp::view
{

namespace
{

std::pair<std::string, nc_color> look_at( map &here, const npc &guy, const tripoint_bub_ms &p )
{
    if( !here.inbounds( p ) || !guy.sees( here, p ) ) {
        return { " ", c_black };
    }
    if( const Creature *critter = get_creature_tracker().creature_at( p ) ) {
        if( guy.sees( here, *critter ) ) {
            return { critter->symbol(), critter->symbol_color() };
        }
    }
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        const vpart_display shown = vp->vehicle().get_display_of_tile( vp->mount_pos() );
        if( shown.symbol != ' ' ) {
            return { utf32_to_utf8( shown.symbol ), shown.color };
        }
    }
    if( here.has_items( p ) ) {
        const map_stack items = here.i_at( p );
        const item &top = *items.begin();
        return { top.symbol(), top.color() };
    }
    if( here.has_furn( p ) ) {
        const furn_t &furn = here.furn( p ).obj();
        return { utf32_to_utf8( furn.symbol() ), furn.color() };
    }
    const ter_t &ter = here.ter( p ).obj();
    return { utf32_to_utf8( ter.symbol() ), ter.color() };
}

} // namespace

void write( JsonOut &json, const npc &guy )
{
    map &here = get_map();
    const tripoint_bub_ms center = guy.pos_bub( here );
    std::map<std::string, int> palette;
    std::vector<std::string> palette_names;

    json.member( "radius", radius );
    json.member( "rows" );
    json.start_array();
    for( int dy = -radius; dy <= radius; dy++ ) {
        json.start_array();
        for( int dx = -radius; dx <= radius; dx++ ) {
            const std::pair<std::string, nc_color> shown =
                look_at( here, guy, center + tripoint_rel_ms( dx, dy, 0 ) );
            const std::string color_name = get_all_colors().get_name( shown.second );
            auto found = palette.find( color_name );
            if( found == palette.end() ) {
                found = palette.emplace( color_name, static_cast<int>( palette_names.size() ) ).first;
                palette_names.push_back( color_name );
            }
            json.start_array();
            json.write( shown.first );
            json.write( found->second );
            json.end_array();
        }
        json.end_array();
    }
    json.end_array();
    json.member( "palette", palette_names );
}

grid read( const JsonObject &message )
{
    message.allow_omitted_members();
    grid result;
    result.radius = message.get_int( "radius" );
    std::vector<std::string> palette;
    for( const std::string name : message.get_array( "palette" ) ) {
        palette.push_back( name );
    }
    for( const JsonArray row : message.get_array( "rows" ) ) {
        std::vector<cell> cells;
        for( const JsonArray entry : row ) {
            cell c;
            c.symbol = entry.get_string( 0 );
            const int index = entry.get_int( 1 );
            if( index >= 0 && static_cast<size_t>( index ) < palette.size() ) {
                c.color = palette[index];
            }
            cells.push_back( std::move( c ) );
        }
        result.rows.push_back( std::move( cells ) );
    }
    return result;
}

} // namespace mp::view
