#include "mp/remote_trade.h"

#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "character.h"
#include "creature_tracker.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "messages.h"
#include "mp/net.h"
#include "mp/protocol.h"
#include "mp/rc_npc.h"
#include "mp/remote_prompt.h"
#include "npc.h"
#include "npctrade.h"
#include "npctrade_utils.h"
#include "output.h"
#include "skill.h"
#include "string_formatter.h"
#include "trade_ui.h"
#include "translations.h"

static const skill_id skill_speech( "speech" );

namespace mp::remote_trade
{

namespace
{

struct result {
    bool traded = false;
    int balance = 0;
    int delta_bank = 0;
    int value_you = 0;
    trade_selector::select_t items_you;
    trade_selector::select_t items_trader;
};

void write_items( JsonOut &json, const std::string &name, const trade_selector::select_t &items )
{
    json.member( name );
    json.start_array();
    for( const trade_selector::entry_t &e : items ) {
        json.start_array();
        json.write( e.first );
        json.write( e.second );
        json.end_array();
    }
    json.end_array();
}

// Only what `owner` has or what lies around: no taking from anyone else.
trade_selector::select_t read_items( const JsonObject &answer, const std::string &name,
                                     const Character &owner )
{
    trade_selector::select_t items;
    if( !answer.has_array( name ) ) {
        return items;
    }
    for( JsonArray entry : answer.get_array( name ) ) {
        item_location loc;
        loc.deserialize( entry.get_object( 0 ) );
        const int count = entry.get_int( 1 );
        if( !loc || count <= 0 ) {
            continue;
        }
        if( loc.carrier() != nullptr && loc.carrier() != &owner ) {
            continue;
        }
        if( loc.carrier() == nullptr && rl_dist( loc.pos_abs(), owner.pos_abs() ) > 60 ) {
            continue;
        }
        items.emplace_back( loc, count );
    }
    return items;
}

// Asks the client to trade with `other` (its copy is found by position).
std::optional<result> ask( const npc &guy, const Character &other, const int cost,
                           const std::string &deal )
{
    const std::optional<std::string> line = remote_prompt::ask_json( "trade",
    [&]( JsonOut & json ) {
        json.member( "at", other.pos_abs() );
        json.member( "cost", cost );
        json.member( "deal", deal );
    } );
    if( !line ) {
        return std::nullopt;
    }
    result res;
    const protocol::reading_network reading;
    try {
        const JsonValue value = json_loader::from_string( *line );
        const JsonObject obj = value.get_object();
        obj.allow_omitted_members();
        res.traded = obj.get_bool( "traded", false );
        if( !res.traded ) {
            return res;
        }
        res.balance = obj.get_int( "balance", 0 );
        res.delta_bank = obj.get_int( "delta_bank", 0 );
        res.value_you = obj.get_int( "value_you", 0 );
        res.items_you = read_items( obj, "items_you", guy );
        res.items_trader = read_items( obj, "items_trader", other );
    } catch( const JsonError & ) {
        return std::nullopt;
    }
    return res;
}

// npc_trading::trade() after the trade screen, guy in the avatar's place.
void apply( npc &guy, Character &other, result &res )
{
    npc *other_npc = other.as_npc();
    std::list<item_location *> from_map;
    // Movement of items in 3 steps: guy to escrow - other to guy - escrow to other.
    std::list<item> escrow = npc_trading::transfer_items( res.items_you, guy, other, from_map,
                             true );
    npc_trading::transfer_items( res.items_trader, other, guy, from_map, false );
    if( other_npc != nullptr && other_npc->is_shopkeeper() ) {
        distribute_items_to_npc_zones( escrow, *other_npc );
    } else {
        for( const item &i : escrow ) {
            other.i_add( i, true, nullptr, nullptr, true, false );
        }
    }
    for( item_location *loc_ptr : from_map ) {
        if( loc_ptr == nullptr ) {
            continue;
        }
        item *it = loc_ptr->get_item();
        if( it == nullptr ) {
            continue;
        }
        if( it->has_var( "trade_charges" ) && it->count_by_charges() ) {
            it->charges -= static_cast<int>( it->get_var( "trade_charges", 0 ) );
            if( it->charges <= 0 ) {
                loc_ptr->remove_item();
            } else {
                it->erase_var( "trade_charges" );
            }
        } else if( it->has_var( "trade_amount" ) ) {
            loc_ptr->remove_item();
        }
    }
    if( other_npc != nullptr && !other_npc->will_exchange_items_freely() ) {
        guy.cash -= res.delta_bank;
        npc_trading::update_npc_owed( *other_npc, res.balance, res.value_you );
        guy.practice( skill_speech, res.value_you / 10000 );
    }
}

std::string describe( const trade_selector::select_t &items )
{
    std::vector<std::string> names;
    for( const trade_selector::entry_t &e : items ) {
        names.push_back( e.second > 1 ? string_format( "%d %s", e.second, e.first->tname( e.second ) ) :
                         e.first->tname() );
    }
    return names.empty() ? std::string( _( "nothing" ) ) : enumerate_as_string( names );
}

} // namespace

bool trade_with_npc( npc &guy, npc &np, const int cost, const std::string &deal )
{
    np.shop_restock();
    np.drop_invalid_inventory();
    std::optional<result> res = ask( guy, np, cost, deal );
    if( !res || !res->traded ) {
        return false;
    }
    apply( guy, np, *res );
    return true;
}

void trade_with_host( npc &guy )
{
    avatar &host = get_avatar();
    std::optional<result> res = ask( guy, host, 0, _( "Trade" ) );
    if( !res || !res->traded ) {
        return;
    }
    if( res->items_you.empty() && res->items_trader.empty() ) {
        return;
    }
    bool agreed = false;
    {
        // The host decides about their own things.
        remote_prompt::host_question asking_host;
        agreed = query_yn( _( "%1$s offers you: %2$s.\nAnd wants from you: %3$s.\nAgree?" ),
                           guy.get_name(), describe( res->items_you ), describe( res->items_trader ) );
    }
    if( !agreed ) {
        guy.add_msg_if_player( m_bad, _( "%s doesn't agree to the exchange." ), host.get_name() );
        return;
    }
    apply( guy, host, *res );
    guy.add_msg_if_player( m_good, _( "You exchange things with %s." ), host.get_name() );
    add_msg( m_good, _( "You exchange things with %s." ), guy.get_name() );
}

bool partner_refuses( const npc &np, const trade_selector::select_t &from_host,
                      const trade_selector::select_t &from_partner )
{
    if( !is_remote( np ) || !net::has_client() ) {
        return false;
    }
    // Asked of the second player: their things.
    remote_prompt::asking_client asking;
    const bool agreed = query_yn( _( "%1$s offers you: %2$s.\nAnd wants from you: %3$s.\nAgree?" ),
                                  get_avatar().get_name(), describe( from_host ), describe( from_partner ) );
    if( !agreed ) {
        add_msg( m_bad, _( "%s doesn't agree to the exchange." ), np.get_name() );
    }
    return !agreed;
}

void answer( const JsonObject &question )
{
    const int id = question.get_int( "id", 0 );
    tripoint_abs_ms at;
    question.read( "at", at );
    npc *trader = get_creature_tracker().creature_at<npc>( at );
    if( trader == nullptr ) {
        popup( _( "You can't see who you trade with." ) );
        remote_prompt::send_answer( id, []( JsonOut & json ) {
            json.member( "traded", false );
        } );
        return;
    }
    // The game's trade screen on the copies.
    trade_ui::trade_result_t res;
    {
        trade_ui ui( get_avatar(), *trader, question.get_int( "cost", 0 ),
                     question.get_string( "deal", _( "Trade" ) ) );
        res = ui.perform_trade();
    }
    remote_prompt::send_answer( id, [&]( JsonOut & json ) {
        json.member( "traded", res.traded );
        json.member( "balance", res.balance );
        json.member( "delta_bank", res.delta_bank );
        json.member( "value_you", res.value_you );
        write_items( json, "items_you", res.items_you );
        write_items( json, "items_trader", res.items_trader );
    } );
}

} // namespace mp::remote_trade
