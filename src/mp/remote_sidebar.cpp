#include "mp/remote_sidebar.h"

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "calendar.h"
#include "character_martial_arts.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "display.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "line.h"
#include "map.h"
#include "martialarts.h"
#include "npc.h"
#include "omdata.h"
#include "output.h"
#include "overmapbuffer.h"
#include "string_formatter.h"
#include "translations.h"
#include "units.h"

namespace mp::remote_sidebar
{

namespace
{

std::string colored( const std::pair<std::string, nc_color> &text_color )
{
    return colorize( text_color.first, text_color.second );
}

std::string label( const std::string &text )
{
    return colorize( text, c_light_gray );
}

// "Name: value" pairs, two per line, like the host's classic sidebar.
void two_columns( std::vector<std::string> &lines, const std::string &left,
                  const std::string &right )
{
    lines.push_back( left + std::string( std::max( 2, 26 - utf8_width( left, true ) ), ' ' ) + right );
}

std::string direction_name( const point_rel_ms &d )
{
    const int x = d.x() > 0 ? 1 : d.x() < 0 ? -1 : 0;
    const int y = d.y() > 0 ? 1 : d.y() < 0 ? -1 : 0;
    static const std::map<std::pair<int, int>, std::string> names = {
        { { 0, -1 }, "N" }, { { 1, -1 }, "NE" }, { { 1, 0 }, "E" }, { { 1, 1 }, "SE" },
        { { 0, 1 }, "S" }, { { -1, 1 }, "SW" }, { { -1, 0 }, "W" }, { { -1, -1 }, "NW" },
    };
    const auto it = names.find( { x, y } );
    return it == names.end() ? "" : it->second;
}

} // namespace

void write( JsonOut &json, const npc &guy )
{
    map &here = get_map();
    std::vector<std::string> lines;

    // Body parts, two per line.
    std::vector<std::string> parts;
    for( const bodypart_id &bp : guy.get_all_body_parts( get_body_part_flags::only_main |
            get_body_part_flags::sorted ) ) {
        const std::pair<std::string, nc_color> bar = get_hp_bar( guy.get_part_hp_cur( bp ),
                guy.get_part_hp_max( bp ) );
        parts.push_back( string_format( "%-7s %s", body_part_hp_bar_ui_text( bp ), colored( bar ) ) );
    }
    for( size_t i = 0; i < parts.size(); i += 2 ) {
        two_columns( lines, parts[i], i + 1 < parts.size() ? parts[i + 1] : "" );
    }

    two_columns( lines, label( _( "Stam:  " ) ) + colored( get_hp_bar( guy.get_stamina(),
                 guy.get_stamina_max() ) ), label( _( "Speed: " ) ) + std::to_string( guy.get_speed() ) );
    two_columns( lines, label( _( "Focus: " ) ) + std::to_string( guy.get_focus() ),
                 label( _( "Move:  " ) ) + std::to_string( guy.get_moves() ) );
    two_columns( lines, label( _( "Str: " ) ) + std::to_string( guy.get_str() ) + "  " +
                 label( _( "Dex: " ) ) + std::to_string( guy.get_dex() ),
                 label( _( "Int: " ) ) + std::to_string( guy.get_int() ) + "  " +
                 label( _( "Per: " ) ) + std::to_string( guy.get_per() ) );
    two_columns( lines, label( _( "Weariness: " ) ) + colored( display::weariness_text_color( guy ) ),
                 label( _( "Activity: " ) ) + colored( display::activity_text_color( guy ) ) );
    two_columns( lines, label( _( "Pain: " ) ) + colored( display::pain_text_color( guy ) ),
                 label( _( "Thirst: " ) ) + colored( display::thirst_text_color( guy ) ) );
    two_columns( lines, label( _( "Rest: " ) ) + colored( display::sleepiness_text_color( guy ) ),
                 label( _( "Hunger: " ) ) + colored( display::hunger_text_color( guy ) ) );
    two_columns( lines, label( _( "Heat: " ) ) + colored( display::temp_text_color( guy ) ),
                 label( _( "Weight: " ) ) + colored( display::weight_text_color( guy ) ) );
    lines.emplace_back();

    const oter_id &omt = overmap_buffer.ter( guy.pos_abs_omt() );
    lines.push_back( label( _( "Place:   " ) ) + colorize( omt->get_name( om_vision_level::full ),
                     omt->get_color( om_vision_level::full ) ) );
    lines.push_back( label( _( "Weather: " ) ) + colored( display::weather_text_color( guy ) ) );
    lines.push_back( label( _( "Wind:    " ) ) + colored( display::wind_text_color( guy ) ) );
    lines.push_back( label( _( "Date:    " ) ) + display::date_string() );
    lines.push_back( label( _( "Time:    " ) ) + display::time_string( guy ) );
    const item_location weapon = guy.get_wielded_item();
    lines.push_back( label( _( "Wield: " ) ) + ( weapon ? weapon->tname() : _( "fists" ) ) );
    lines.push_back( label( _( "Style: " ) ) +
                     guy.martial_arts_data->selected_style_name( guy ) );
    lines.emplace_back();

    // What the character sees around, by direction, like the host's compass.
    const tripoint_bub_ms pos = guy.pos_bub( here );
    std::map<std::string, std::vector<std::string>> seen;
    for( Creature &critter : g->all_creatures() ) {
        if( &critter == &guy || critter.posz() != guy.posz() || !guy.sees( here, critter ) ) {
            continue;
        }
        const point_rel_ms d = ( critter.pos_bub( here ) - pos ).xy();
        if( std::max( std::abs( d.x() ), std::abs( d.y() ) ) > 60 ) {
            continue;
        }
        // disp_name() of the host's character from here would be "you".
        const std::string name = critter.is_monster() ? critter.disp_name() :
                                 critter.as_character()->get_name();
        seen[direction_name( d )].push_back( colorize( name,
                                             critter.is_monster() && critter.attitude_to( guy ) == Creature::Attitude::HOSTILE ?
                                             c_red : c_white ) );
    }
    static const std::array<std::string, 8> order = { "NW", "N", "NE", "W", "E", "SW", "S", "SE" };
    bool any = false;
    for( const std::string &dir : order ) {
        const auto it = seen.find( dir );
        if( it == seen.end() ) {
            continue;
        }
        any = true;
        std::string line = label( dir + ": " );
        for( size_t i = 0; i < it->second.size(); i++ ) {
            line += ( i > 0 ? ", " : "" ) + it->second[i];
        }
        lines.push_back( line );
    }
    if( !any ) {
        lines.push_back( label( _( "Nobody in sight." ) ) );
    }

    json.member( "lines", lines );
}

} // namespace mp::remote_sidebar
