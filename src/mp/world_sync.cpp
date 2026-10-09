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
#include "mp/npc_grab.h"
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

static const faction_id faction_your_followers( "your_followers" );

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
// The same without the position.
size_t sent_character_still = 0;

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
    sent_character_still = 0;
    sent_overmap = 0;
}

bool write_creatures( JsonOut &json, const npc &guy )
{
    std::vector<std::pair<std::string, std::string>> list;
    // Which way sprites face is not saved with the creature.
    std::vector<bool> left;
    const auto add = [&]( const std::string & kind, const Creature & critter, std::string text ) {
        list.emplace_back( kind, std::move( text ) );
        left.push_back( critter.facing == FacingDirection::LEFT );
    };
    for( const monster &mon : g->all_monsters() ) {
        if( guy.sees( get_map(), mon ) ) {
            add( "monster", mon, to_text( mon ) );
        }
    }
    for( const npc &other : g->all_npcs() ) {
        if( &other != &guy && guy.sees( get_map(), other ) ) {
            add( "npc", other, to_text( other ) );
        }
    }
    // The host: an NPC on the client.
    if( guy.sees( get_map(), get_avatar() ) ) {
        add( "host", get_avatar(), to_text( get_avatar() ) );
    }
    const bool self_left = guy.facing == FacingDirection::LEFT;
    size_t hash = list.size() * 2 + ( self_left ? 1 : 0 );
    for( size_t i = 0; i < list.size(); i++ ) {
        hash = hash * 31 + std::hash<std::string>()( list[i].second ) + ( left[i] ? 7 : 0 );
    }
    if( hash == sent_creatures ) {
        return false;
    }
    sent_creatures = hash;
    json.member( "self_left", self_left );
    json.member( "list" );
    json.start_array();
    for( size_t i = 0; i < list.size(); i++ ) {
        const auto &e = list[i];
        json.start_object();
        json.member( "kind", e.first );
        json.member( "left", static_cast<bool>( left[i] ) );
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

// The character's JSON without its "location" member.
static std::string without_location( const std::string &data )
{
    const size_t start = data.find( "\"location\":[" );
    if( start == std::string::npos ) {
        return data;
    }
    const size_t end = data.find( ']', start );
    if( end == std::string::npos ) {
        return data;
    }
    return data.substr( 0, start ) + data.substr( end + 1 );
}

std::string character_if_changed( const npc &guy, const bool force, bool *moved )
{
    if( moved != nullptr ) {
        *moved = false;
    }
    std::string data = to_text( guy );
    // The client loads it into an avatar, which also wants a scenario.
    if( !data.empty() && data[0] == '{' && get_scenario() != nullptr ) {
        data.insert( 1, "\"scenario\":\"" + get_scenario()->ident().str() + "\"," );
    }
    npc_grab::add_to_json( data, guy );
    const size_t hash = std::hash<std::string>()( data );
    if( !force && hash == sent_character ) {
        return std::string();
    }
    const size_t still = std::hash<std::string>()( without_location( data ) );
    sent_character = hash;
    if( !force && moved != nullptr && still == sent_character_still ) {
        *moved = true;
        return std::string();
    }
    sent_character_still = still;
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

// The host's do_turn() does this every turn. The client's map is centred
// on the character, so its position in the map rarely changes and
// map::update_visibility_cache() would never notice the move.
static void invalidate_view()
{
    map &here = get_map();
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
        here.invalidate_map_cache( z );
    }
    here.invalidate_visibility_cache();
}

void read( const JsonObject &message )
{
    message.allow_omitted_members();
    calendar::turn = calendar::turn_zero + time_duration::from_turns( message.get_int( "turn", 0 ) );
    // A submap replaced below takes its vehicles with it: the map must not
    // keep pointers to them (map::load() lists the new ones).
    map &here = get_map();
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
        here.clear_vehicle_list( z );
    }
    here.clear_vehicle_level_caches();
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
    invalidate_view();
}

void read_creatures( const JsonObject &message )
{
    message.allow_omitted_members();
    creature_tracker &tracker = get_creature_tracker();
    // Only copies live here: replaced by the new ones.
    tracker.clear();
    tracker.clear_npcs();
    get_avatar().facing = message.get_bool( "self_left", false ) ? FacingDirection::LEFT :
                          FacingDirection::RIGHT;
    for( JsonObject e : message.get_array( "list" ) ) {
        e.allow_omitted_members();
        const std::string kind = e.get_string( "kind", "" );
        const FacingDirection facing = e.get_bool( "left", false ) ? FacingDirection::LEFT :
                                       FacingDirection::RIGHT;
        if( kind == "monster" ) {
            auto mon = make_shared_fast<monster>();
            mon->deserialize( e.get_object( "data" ) );
            mon->facing = facing;
            tracker.add( mon );
        } else if( kind == "npc" || kind == "host" ) {
            auto guy = make_shared_fast<npc>();
            guy->deserialize( e.get_object( "data" ) );
            if( kind == "host" ) {
                // The other player: an ally, not a stranger (free exchange of
                // things, as with followers).
                guy->set_attitude( NPCATT_FOLLOW );
                guy->set_fac( faction_your_followers );
            }
            guy->facing = facing;
            tracker.add_npc( guy );
        }
    }
}

void read_position( const JsonObject &message )
{
    message.allow_omitted_members();
    JsonArray at = message.get_array( "at" );
    const int x = at.next_int();
    const int y = at.next_int();
    const int z = at.next_int();
    get_avatar().setpos( tripoint_abs_ms( x, y, z ), false );
    follow_avatar();
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
    // Daylight changes what is seen.
    get_map().invalidate_visibility_cache();
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
    get_avatar().recalc_sight_limits();
    const tripoint_abs_sm at = project_to<coords::sm>( get_avatar().pos_abs() );
    const tripoint_abs_sm center = here.get_abs_sub() + point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE );
    if( at.xy() != center.xy() || at.z() != here.get_abs_sub().z() ) {
        here.load( tripoint_abs_sm( at.xy() - point_rel_sm( HALF_MAPSIZE, HALF_MAPSIZE ), at.z() ),
                   true );
    }
    invalidate_view();
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
