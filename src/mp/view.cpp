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
#include "field.h"
#include "item.h"
#include "monster.h"
#include "mtype.h"
#include "json.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace mp::view
{

namespace
{

// Ids of what is on a tile, for drawing it with tiles on the client.
struct layers {
    std::string ter;
    std::string furn;
    std::string field;
    int field_intensity = 0;
    std::string item;
    std::string vpart;
    std::string vpart_variant;
    std::string critter;
};

layers layers_at( map &here, const npc &guy, const tripoint_bub_ms &p )
{
    layers l;
    if( !here.inbounds( p ) || !guy.sees( here, p ) ) {
        return l;
    }
    l.ter = here.ter( p ).id().str();
    if( here.has_furn( p ) ) {
        l.furn = here.furn( p ).id().str();
    }
    const field &fields = here.field_at( p );
    const field_type_id shown_field = fields.displayed_field_type();
    if( shown_field && !shown_field.id().is_null() ) {
        l.field = shown_field.id().str();
        l.field_intensity = fields.displayed_intensity();
    }
    if( here.has_items( p ) ) {
        const map_stack items = here.i_at( p );
        l.item = items.begin()->typeId().str();
    }
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        const vpart_display shown = vp->vehicle().get_display_of_tile( vp->mount_pos() );
        l.vpart = "vp_" + shown.id.str();
        l.vpart_variant = shown.variant.id;
    }
    if( const Creature *critter = get_creature_tracker().creature_at( p ) ) {
        if( guy.sees( here, *critter ) ) {
            if( const monster *mon = critter->as_monster() ) {
                l.critter = mon->type->id.str();
            } else if( const Character *ch = critter->as_character() ) {
                // The second player is the "player" on their own screen.
                l.critter = std::string( ch == &guy ? "player_" : "npc_" ) +
                            ( ch->male ? "male" : "female" );
            }
        }
    }
    return l;
}

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
    // Every id string is sent once; cells refer to it by index, -1 is none.
    std::map<std::string, int> id_index;
    std::vector<std::string> ids;
    const auto index_of = [&]( const std::string & id ) {
        if( id.empty() ) {
            return -1;
        }
        auto found = id_index.find( id );
        if( found == id_index.end() ) {
            found = id_index.emplace( id, static_cast<int>( ids.size() ) ).first;
            ids.push_back( id );
        }
        return found->second;
    };

    json.member( "radius", radius );
    json.member( "rows" );
    json.start_array();
    for( int dy = -radius; dy <= radius; dy++ ) {
        json.start_array();
        for( int dx = -radius; dx <= radius; dx++ ) {
            const tripoint_bub_ms p = center + tripoint_rel_ms( dx, dy, 0 );
            const std::pair<std::string, nc_color> shown = look_at( here, guy, p );
            const layers l = layers_at( here, guy, p );
            const std::string color_name = get_all_colors().get_name( shown.second );
            auto found = palette.find( color_name );
            if( found == palette.end() ) {
                found = palette.emplace( color_name, static_cast<int>( palette_names.size() ) ).first;
                palette_names.push_back( color_name );
            }
            json.start_array();
            json.write( shown.first );
            json.write( found->second );
            if( !l.ter.empty() ) {
                json.write( index_of( l.ter ) );
                json.write( index_of( l.furn ) );
                json.write( index_of( l.field ) );
                json.write( l.field_intensity );
                json.write( index_of( l.item ) );
                json.write( index_of( l.vpart ) );
                json.write( index_of( l.vpart_variant ) );
                json.write( index_of( l.critter ) );
            }
            json.end_array();
        }
        json.end_array();
    }
    json.end_array();
    json.member( "palette", palette_names );
    json.member( "ids", ids );
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
    std::vector<std::string> ids;
    if( message.has_array( "ids" ) ) {
        for( const std::string id : message.get_array( "ids" ) ) {
            ids.push_back( id );
        }
    }
    const auto id_at = [&ids]( const int index ) {
        return index >= 0 && static_cast<size_t>( index ) < ids.size() ? ids[index] : std::string();
    };
    for( const JsonArray row : message.get_array( "rows" ) ) {
        std::vector<cell> cells;
        for( const JsonArray entry : row ) {
            cell c;
            c.symbol = entry.get_string( 0 );
            const int index = entry.get_int( 1 );
            if( index >= 0 && static_cast<size_t>( index ) < palette.size() ) {
                c.color = palette[index];
            }
            if( entry.size() >= 10 ) {
                c.ter = id_at( entry.get_int( 2 ) );
                c.furn = id_at( entry.get_int( 3 ) );
                c.field = id_at( entry.get_int( 4 ) );
                c.field_intensity = entry.get_int( 5 );
                c.item = id_at( entry.get_int( 6 ) );
                c.vpart = id_at( entry.get_int( 7 ) );
                c.vpart_variant = id_at( entry.get_int( 8 ) );
                c.critter = id_at( entry.get_int( 9 ) );
            }
            cells.push_back( std::move( c ) );
        }
        result.rows.push_back( std::move( cells ) );
    }
    return result;
}

} // namespace mp::view
