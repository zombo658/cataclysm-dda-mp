#include "mp/remote_inventory.h"

#include <list>
#include <string>
#include <utility>
#include <vector>

#include "character.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "npc.h"
#include "pocket_type.h"
#include "translations.h"

namespace mp::inventory
{

namespace
{

// The items of the last list sent, by number.
std::vector<item_location> listed;
int listed_revision = 0;

std::vector<std::string> actions_for( const npc &guy, const item &it )
{
    std::vector<std::string> actions;
    if( guy.is_wielding( it ) ) {
        actions.emplace_back( "unwield" );
    } else {
        actions.emplace_back( "wield" );
    }
    if( guy.is_worn( it ) ) {
        actions.emplace_back( "takeoff" );
    } else if( it.is_armor() ) {
        actions.emplace_back( "wear" );
    }
    if( it.is_comestible() ) {
        actions.emplace_back( "eat" );
    }
    actions.emplace_back( "drop" );
    return actions;
}

void write_entry( JsonOut &json, const npc &guy, const item_location &loc, const std::string &where,
                  const int depth )
{
    json.start_object();
    json.member( "index", static_cast<int>( listed.size() ) );
    json.member( "name", loc->display_name() );
    json.member( "where", where );
    json.member( "depth", depth );
    json.member( "actions", actions_for( guy, *loc ) );
    json.end_object();
    listed.push_back( loc );
}

// The item and, depth first, everything stored in it.
void write_tree( JsonOut &json, const npc &guy, item_location loc, const std::string &where,
                 const int depth )
{
    write_entry( json, guy, loc, where, depth );
    for( item *content : loc->all_items_top( pocket_type::CONTAINER ) ) {
        write_tree( json, guy, item_location( loc, content ), loc->tname(), depth + 1 );
    }
}

} // namespace

void write( JsonOut &json, npc &guy )
{
    listed.clear();
    listed_revision++;
    json.member( "revision", listed_revision );
    json.member( "items" );
    json.start_array();
    if( const item_location weapon = guy.get_wielded_item() ) {
        write_tree( json, guy, weapon, "wielded", 0 );
    }
    for( const item_location &top : guy.top_items_loc() ) {
        write_tree( json, guy, top, guy.is_worn( *top ) ? "worn" : "carried", 0 );
    }
    json.end_array();
}

std::string act( npc &guy, const int revision, const int index, const std::string &action )
{
    if( revision != listed_revision || index < 0 || index >= static_cast<int>( listed.size() ) ) {
        return _( "the inventory has changed, look again" );
    }
    item_location loc = listed[index];
    if( !loc || loc.get_item() == nullptr ) {
        return _( "that item is gone" );
    }
    // Every list is good for one action: after it the items may have moved.
    listed_revision++;
    const item &it = *loc;
    if( action == "wield" ) {
        if( !guy.wield( loc ) ) {
            return _( "can't wield that" );
        }
    } else if( action == "unwield" ) {
        if( !guy.is_wielding( it ) || !guy.unwield() ) {
            return _( "can't put that away" );
        }
    } else if( action == "wear" ) {
        if( !it.is_armor() || !guy.wear( loc ) ) {
            return _( "can't wear that" );
        }
    } else if( action == "takeoff" ) {
        if( !guy.is_worn( it ) || !guy.takeoff( loc ) ) {
            return _( "can't take that off" );
        }
    } else if( action == "eat" ) {
        if( !it.is_comestible() || guy.consume( loc ) == trinary::NONE ) {
            return _( "can't eat or drink that" );
        }
    } else if( action == "drop" ) {
        drop_locations what;
        what.emplace_back( loc, it.count() );
        guy.drop( what, guy.pos_bub(), false );
    } else {
        return _( "unknown action" );
    }
    return std::string();
}

listing read( const JsonObject &message )
{
    message.allow_omitted_members();
    listing result;
    result.revision = message.get_int( "revision", 0 );
    for( const JsonObject e : message.get_array( "items" ) ) {
        e.allow_omitted_members();
        entry en;
        en.index = e.get_int( "index" );
        en.name = e.get_string( "name", "" );
        en.where = e.get_string( "where", "" );
        en.depth = e.get_int( "depth", 0 );
        for( const std::string a : e.get_array( "actions" ) ) {
            en.actions.push_back( a );
        }
        result.entries.push_back( std::move( en ) );
    }
    return result;
}

} // namespace mp::inventory
