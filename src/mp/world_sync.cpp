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
#include "avatar.h"
#include "creature_tracker.h"
#include "monster.h"
#include "npc.h"
#include "omdata.h"
#include "overmapbuffer.h"
#include "scenario.h"
#include "units.h"
#include "weather.h"
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
size_t sent_creatures = 0;
size_t sent_overmap = 0;
// Overmap terrain around the character to keep in sync (the minimap shows
// a few tiles each way, the overmap screen more).
constexpr int overmap_radius = 12;
size_t sent_character = 0;

template<typename T>
std::string to_text( const T &thing )
{
    std::ostringstream os;
    JsonOut json( os );
    thing.serialize( json );
    return os.str();
}

void write_raw( JsonOut &json, const std::string &text )
{
    if( json.get_need_separator() ) {
        json.write_separator();
    }
    *json.get_stream() << text;
    json.set_need_separator();
}

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
    sent_creatures = 0;
    sent_character = 0;
    sent_overmap = 0;
}

bool write_creatures( JsonOut &json, const npc &guy )
{
    std::vector<std::pair<std::string, std::string>> list;
    for( const monster &mon : g->all_monsters() ) {
        if( guy.sees( get_map(), mon ) ) {
            list.emplace_back( "monster", to_text( mon ) );
        }
    }
    for( const npc &other : g->all_npcs() ) {
        if( &other != &guy && guy.sees( get_map(), other ) ) {
            list.emplace_back( "npc", to_text( other ) );
        }
    }
    // The host: an NPC on the client.
    if( guy.sees( get_map(), get_avatar() ) ) {
        list.emplace_back( "host", to_text( get_avatar() ) );
    }
    size_t hash = list.size();
    for( const auto &e : list ) {
        hash = hash * 31 + std::hash<std::string>()( e.second );
    }
    if( hash == sent_creatures ) {
        return false;
    }
    sent_creatures = hash;
    json.member( "list" );
    json.start_array();
    for( const auto &e : list ) {
        json.start_object();
        json.member( "kind", e.first );
        json.member( "data" );
        write_raw( json, e.second );
        json.end_object();
    }
    json.end_array();
    return true;
}

bool write_overmap( JsonOut &json, const npc &guy )
{
    const tripoint_abs_omt center = project_to<coords::omt>( guy.pos_abs() );
    std::vector<std::string> ids;
    std::vector<int> seen;
    size_t hash = std::hash<std::string>()( center.to_string() );
    for( int dy = -overmap_radius; dy <= overmap_radius; dy++ ) {
        for( int dx = -overmap_radius; dx <= overmap_radius; dx++ ) {
            const tripoint_abs_omt p = center + point_rel_omt( dx, dy );
            ids.push_back( overmap_buffer.ter( p ).id().str() );
            seen.push_back( static_cast<int>( overmap_buffer.seen( p ) ) );
            hash = hash * 31 + std::hash<std::string>()( ids.back() ) + seen.back();
        }
    }
    if( hash == sent_overmap ) {
        return false;
    }
    sent_overmap = hash;
    json.member( "center" );
    json.start_array();
    json.write( center.x() );
    json.write( center.y() );
    json.write( center.z() );
    json.end_array();
    json.member( "radius", overmap_radius );
    json.member( "ids", ids );
    json.member( "seen", seen );
    return true;
}

void write_world( JsonOut &json )
{
    const weather_manager &weather = get_weather();
    json.member( "turn", to_turns<int>( calendar::turn - calendar::turn_zero ) );
    json.member( "weather", weather.weather_id.str() );
    json.member( "temperature", units::to_kelvin( weather.temperature ) );
    json.member( "windspeed", weather.windspeed );
    json.member( "winddirection", weather.winddirection );
    json.member( "lightning", weather.lightning_active );
}

std::string character_if_changed( const npc &guy, const bool force )
{
    std::string data = to_text( guy );
    // The client loads it into an avatar, which also wants a scenario.
    if( !data.empty() && data[0] == '{' && get_scenario() != nullptr ) {
        data.insert( 1, "\"scenario\":\"" + get_scenario()->ident().str() + "\"," );
    }
    const size_t hash = std::hash<std::string>()( data );
    if( !force && hash == sent_character ) {
        return std::string();
    }
    sent_character = hash;
    return data;
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
        write_raw( json, text );
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

void read_creatures( const JsonObject &message )
{
    message.allow_omitted_members();
    creature_tracker &tracker = get_creature_tracker();
    // Only copies live here: replaced by the new ones.
    tracker.clear();
    tracker.clear_npcs();
    for( JsonObject e : message.get_array( "list" ) ) {
        e.allow_omitted_members();
        const std::string kind = e.get_string( "kind", "" );
        if( kind == "monster" ) {
            auto mon = make_shared_fast<monster>();
            mon->deserialize( e.get_object( "data" ) );
            tracker.add( mon );
        } else if( kind == "npc" || kind == "host" ) {
            auto guy = make_shared_fast<npc>();
            guy->deserialize( e.get_object( "data" ) );
            if( kind == "host" ) {
                // The other player: an ally, not a stranger.
                guy->set_attitude( NPCATT_FOLLOW );
            }
            tracker.add_npc( guy );
        }
    }
}

void read_world( const JsonObject &message )
{
    message.allow_omitted_members();
    calendar::turn = calendar::turn_zero + time_duration::from_turns( message.get_int( "turn", 0 ) );
    weather_manager &weather = get_weather();
    const weather_type_id id( message.get_string( "weather", "" ) );
    if( id.is_valid() ) {
        weather.weather_id = id;
    }
    weather.temperature = units::from_kelvin( static_cast<float>( message.get_float( "temperature", 0 ) ) );
    weather.windspeed = message.get_int( "windspeed", 0 );
    weather.winddirection = message.get_int( "winddirection", 0 );
    weather.lightning_active = message.get_bool( "lightning", false );
}

void read_overmap( const JsonObject &message )
{
    message.allow_omitted_members();
    JsonArray c = message.get_array( "center" );
    const int x = c.next_int();
    const int y = c.next_int();
    const int z = c.next_int();
    const tripoint_abs_omt center( x, y, z );
    const int r = message.get_int( "radius" );
    JsonArray ids = message.get_array( "ids" );
    JsonArray seen = message.get_array( "seen" );
    for( int dy = -r; dy <= r; dy++ ) {
        for( int dx = -r; dx <= r; dx++ ) {
            const tripoint_abs_omt p = center + point_rel_omt( dx, dy );
            const oter_str_id id( ids.next_string() );
            const int vision = seen.next_int();
            if( id.is_valid() ) {
                overmap_buffer.ter_set( p, id.id() );
            }
            overmap_buffer.set_seen( p, static_cast<om_vision_level>( vision ) );
        }
    }
}

void follow_avatar()
{
    map &here = get_map();
    const tripoint_abs_sm at = project_to<coords::sm>( get_avatar().pos_abs() );
    const tripoint_abs_sm center = here.get_abs_sub() + point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE );
    if( at.xy() != center.xy() || at.z() != here.get_abs_sub().z() ) {
        here.load( tripoint_abs_sm( at.xy() - point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE ), at.z() ),
                   true );
    }
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
