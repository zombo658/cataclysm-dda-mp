#if defined(TILES)

#include <array>
#include <bitset>
#include <climits>
#include <string>
#include <vector>

#include "cata_tiles.h"
#include "cursesdef.h"
#include "mp/world_sync.h"
#include "avatar.h"
#include "cata_utility.h"
#include "coordinates.h"
#include "game_constants.h"
#include "lightmap.h"
#include "mapdata.h"
#include "mp/view.h"
#include "point.h"
#include "sdl_geometry.h"
#include "sdl_wrappers.h"

namespace
{

const mp::view::cell *cell_at( const mp::view::grid &grid, const point &p )
{
    if( p.y < 0 || p.y >= static_cast<int>( grid.rows.size() ) || p.x < 0 ||
        p.x >= static_cast<int>( grid.rows[p.y].size() ) ) {
        return nullptr;
    }
    return &grid.rows[p.y][p.x];
}

// Bits of the four neighbours (south, east, west, north, as map::offsets)
// that the tile connects to: those in its connect groups, or with the same id
// if it has no groups. Like cata_tiles::get_connect_values(), but on the grid
// from the host instead of the map.
template<typename T>
char connections( const mp::view::grid &grid, const point &p, const std::string &id,
                  std::string mp::view::cell::*layer )
{
    const string_id<T> sid( id );
    if( !sid.is_valid() ) {
        return 0;
    }
    const std::bitset<NUM_TERCONN> &groups = sid->connect_to_groups;
    char val = 0;
    for( int i = 0; i < 4; ++i ) {
        const mp::view::cell *n = cell_at( grid, p + offsets[i] );
        if( n == nullptr || ( n->*layer ).empty() ) {
            continue;
        }
        bool connects = false;
        if( groups.any() ) {
            const string_id<T> nid( n->*layer );
            connects = nid.is_valid() && nid->in_connect_groups( groups );
        } else {
            connects = n->*layer == id;
        }
        if( connects ) {
            val += 1 << i;
        }
    }
    return val;
}

} // namespace

void cata_tiles::draw_remote_view( const point &dest, int width, int height,
                                   const mp::view::grid &grid )
{
    SDL_Rect clip_rect = { dest.x, dest.y, width, height };
    printErrorIf( SDL_RenderSetClipRect( renderer.get(), &clip_rect ) != 0,
                  "SDL_RenderSetClipRect failed" );
    geometry->rect( renderer, clip_rect, SDL_Color() );

    const point s = get_window_base_tile_counts( point( width, height ) );
    const point center( grid.radius, grid.radius );
    o = is_isometric() ? center : center - point( s.x / 2, s.y / 2 );
    op = dest;
    screentile_width = s.x;
    screentile_height = s.y;

    for( int y = 0; y < static_cast<int>( grid.rows.size() ); y++ ) {
        for( int x = 0; x < static_cast<int>( grid.rows[y].size() ); x++ ) {
            const mp::view::cell &c = grid.rows[y][x];
            if( c.ter.empty() ) {
                continue;
            }
            // Cells are drawn at the lowest z-level, so that the fallback for
            // missing sprites doesn't look into the (unloaded) map.
            const tripoint_bub_ms pos( x, y, -OVERMAP_DEPTH );
            int height_3d = 0;
            int subtile = 0;
            int rota = 0;
            get_rotation_and_subtile( connections<ter_t>( grid, point( x, y ), c.ter,
                                      &mp::view::cell::ter ), CHAR_MAX, rota, subtile );
            draw_from_id_string( c.ter, TILE_CATEGORY::TERRAIN, "", pos, subtile, rota,
                                 lit_level::LIT, false, height_3d );
            if( !c.furn.empty() ) {
                get_rotation_and_subtile( connections<furn_t>( grid, point( x, y ), c.furn,
                                          &mp::view::cell::furn ), CHAR_MAX, rota, subtile );
                draw_from_id_string( c.furn, TILE_CATEGORY::FURNITURE, "", pos, subtile, rota,
                                     lit_level::LIT, false, height_3d );
            }
            if( !c.field.empty() ) {
                draw_from_id_string( c.field, TILE_CATEGORY::FIELD, "", pos, 0, 0,
                                     lit_level::LIT, false, height_3d, c.field_intensity );
            }
            if( !c.item.empty() ) {
                draw_from_id_string( c.item, TILE_CATEGORY::ITEM, "", pos, 0, 0,
                                     lit_level::LIT, false, height_3d );
            }
            if( !c.vpart.empty() ) {
                draw_from_id_string( c.vpart, TILE_CATEGORY::VEHICLE_PART, "", pos, 0, 0,
                                     lit_level::LIT, false, height_3d, 0, c.vpart_variant );
            }
            if( !c.critter.empty() ) {
                const bool character = string_starts_with( c.critter, "player_" ) ||
                                       string_starts_with( c.critter, "npc_" );
                draw_from_id_string( c.critter, character ? TILE_CATEGORY::NONE : TILE_CATEGORY::MONSTER,
                                     "", pos, 0, 0, lit_level::LIT, false, height_3d );
            }
        }
    }
    printErrorIf( SDL_RenderSetClipRect( renderer.get(), nullptr ) != 0,
                  "SDL_RenderSetClipRect failed" );
}


void cata_tiles::draw_player_marks( std::multimap<point, formatted_text> &overlay_strings )
{
    const tripoint_bub_ms view = get_player_character().pos_bub() + get_player_character().view_offset;
    for( const mp::world_sync::player_mark &mark : mp::world_sync::players() ) {
        const tripoint_bub_ms p = mark.who->pos_bub();
        if( p.z() != view.z() ) {
            continue;
        }
        // Above the head: the middle of the upper part of the tile.
        const point at = player_to_screen( p.xy() ) + point( tile_width / 2, -tile_height / 4 );
        // Green for the host, cyan for the second player, on both screens.
        const int color = 8 + ( mark.host ? catacurses::green : catacurses::cyan );
        overlay_strings.emplace( at, formatted_text( "\xE2\x96\xBC", color, text_alignment::center ) );
    }
}

#endif // TILES
