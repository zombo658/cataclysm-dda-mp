#include "mp/world_sync.h"

#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "calendar.h"
#include "game.h"
#include "game_constants.h"
#include "json.h"
#include "map.h"
#include "mapbuffer.h"
#include "mp/remote_actions.h"
#include "npc.h"
#include "submap.h"

namespace mp::world_sync
{

namespace
{

// Submaps around the character to keep in sync: enough for anything the
// character can see or reach.
constexpr int radius = 3;

// What was sent, as hashes of the submaps' savegame text.
std::map<tripoint_abs_sm, size_t> sent;

std::string store( const tripoint_abs_sm &pos, submap &sm )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "version", savegame_version );
    json.member( "coordinates" );
    json.start_array();
    json.write( pos.x() );
    json.write( pos.y() );
    json.write( pos.z() );
    json.end_array();
    sm.store( json );
    json.end_object();
    return os.str();
}

} // namespace

void reset()
{
    sent.clear();
}

bool write_changed( JsonOut &json, const npc &guy )
{
    const tripoint_abs_sm center = project_to<coords::sm>( guy.pos_abs() );
    std::vector<std::string> changed;
    for( int z = std::max( -OVERMAP_DEPTH, center.z() - 1 );
         z <= std::min( OVERMAP_HEIGHT, center.z() + 1 ); z++ ) {
        for( int dy = -radius; dy <= radius; dy++ ) {
            for( int dx = -radius; dx <= radius; dx++ ) {
                const tripoint_abs_sm pos( center.x() + dx, center.y() + dy, z );
                if( !MAPBUFFER.submap_exists( pos ) ) {
                    continue;
                }
                submap *sm = MAPBUFFER.lookup_submap( pos );
                if( sm == nullptr ) {
                    continue;
                }
                std::string text = store( pos, *sm );
                const size_t hash = std::hash<std::string>()( text );
                const auto it = sent.find( pos );
                if( it != sent.end() && it->second == hash ) {
                    continue;
                }
                sent[pos] = hash;
                changed.push_back( std::move( text ) );
            }
        }
    }
    if( changed.empty() ) {
        return false;
    }
    json.member( "turn", to_turns<int>( calendar::turn - calendar::turn_zero ) );
    json.member( "center" );
    json.start_array();
    json.write( center.x() );
    json.write( center.y() );
    json.write( center.z() );
    json.end_array();
    json.member( "list" );
    json.start_array();
    for( const std::string &text : changed ) {
        // Already JSON: written as it is.
        if( json.get_need_separator() ) {
            json.write_separator();
        }
        *json.get_stream() << text;
        json.set_need_separator();
    }
    json.end_array();
    return true;
}

void read( const JsonObject &message )
{
    message.allow_omitted_members();
    calendar::turn = calendar::turn_zero + time_duration::from_turns( message.get_int( "turn", 0 ) );
    for( JsonObject submap_json : message.get_array( "list" ) ) {
        submap_json.allow_omitted_members();
        auto sm = std::make_unique<submap>();
        tripoint_abs_sm pos;
        const int version = submap_json.get_int( "version", 0 );
        for( JsonMember member : submap_json ) {
            const std::string name = member.name();
            if( name == "coordinates" ) {
                JsonArray coords = member;
                // One by one: the order of evaluating arguments is unspecified.
                const int x = coords.next_int();
                const int y = coords.next_int();
                const int z = coords.next_int();
                pos = tripoint_abs_sm( x, y, z );
            } else if( name != "version" ) {
                sm->load( member, name, version );
            }
        }
        if( MAPBUFFER.submap_exists( pos ) ) {
            // In place: the map keeps pointers to its submaps.
            *MAPBUFFER.lookup_submap( pos ) = std::move( *sm );
        } else {
            MAPBUFFER.add_submap( pos, sm );
        }
    }
    JsonArray c = message.get_array( "center" );
    const int x = c.next_int();
    const int y = c.next_int();
    const int z = c.next_int();
    const tripoint_abs_sm center( x, y, z );
    // The character in the middle of the map, as the host's bubble has its avatar.
    get_map().load( center - point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE ), true );
}

bool fill_missing( const tripoint_abs_sm &omt_base )
{
    if( !remote_actions::client_active() ) {
        return false;
    }
    for( int dx = 0; dx <= 1; dx++ ) {
        for( int dy = 0; dy <= 1; dy++ ) {
            for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
                const tripoint_abs_sm pos( omt_base.x() + dx, omt_base.y() + dy, z );
                if( !MAPBUFFER.submap_exists( pos ) ) {
                    auto sm = std::make_unique<submap>();
                    MAPBUFFER.add_submap( pos, sm );
                }
            }
        }
    }
    return true;
}

} // namespace mp::world_sync
