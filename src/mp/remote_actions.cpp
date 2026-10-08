#include "mp/remote_actions.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodygraph.h"
#include "debug.h"
#include "game_inventory.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "messages.h"
#include "json.h"
#include "json_loader.h"
#include "line.h"
#include "map.h"
#include "mp/net.h"
#include "npc.h"
#include "output.h"
#include "string_formatter.h"
#include "translations.h"
#include "uilist.h"

namespace mp::remote_actions
{

namespace
{

bool client_on = false;

item_location read_location( const JsonObject &request, const std::string &name )
{
    item_location loc;
    if( request.has_object( name ) ) {
        loc.deserialize( request.get_object( name ) );
    }
    return loc;
}

// The host's code of the item menu (game::inventory_item_menu()) for a
// character that is not the avatar.
std::string do_item_action( npc &guy, item_location loc, const int key, const JsonObject &request )
{
    switch( key ) {
        case 'W':
            guy.wear( loc );
            break;
        case 'w':
            if( !guy.can_wield( *loc ).success() ) {
                return guy.can_wield( *loc ).str();
            }
            guy.wield( loc );
            break;
        case 'c':
            guy.change_side( loc );
            break;
        case 'T':
            guy.takeoff( loc.obtain( guy ) );
            break;
        case 'd':
            if( request.has_array( "items" ) ) {
                drop_locations what;
                for( JsonArray entry : request.get_array( "items" ) ) {
                    item_location it;
                    it.deserialize( entry.get_object( 0 ) );
                    if( it && it.carrier() == &guy ) {
                        what.emplace_back( it, entry.get_int( 1 ) );
                    }
                }
                guy.drop( what, guy.pos_bub(), false );
            } else {
                guy.Character::drop( loc, guy.pos_bub() );
            }
            break;
        case 'E':
            guy.consume( loc );
            break;
        case 'U':
            guy.unload( loc );
            break;
        case 'D':
            guy.disassemble( loc, false );
            break;
        case 'm':
            guy.mend_item( std::move( loc ), false );
            break;
        case 'a': {
            const std::string method = request.get_string( "method", "" );
            if( method.empty() ) {
                guy.invoke_item( loc.get_item() );
            } else {
                guy.invoke_item( loc.get_item(), method );
            }
            break;
        }
        case 'R': {
            std::vector<std::string> reasons;
            const Character *reader = guy.get_book_reader( *loc, reasons );
            if( reader == nullptr ) {
                return reasons.empty() ? std::string( _( "you can't read that" ) ) : reasons.front();
            }
            item_location no_ereader;
            guy.assign_activity( read_activity_actor( guy.time_to_read( *loc, *reader ), loc,
                                 no_ereader, false ) );
            break;
        }
        case 'r':
        case 'p': {
            const item_location ammo = read_location( request, "ammo" );
            if( !ammo ) {
                return _( "no ammo chosen" );
            }
            item::reload_option opt( &guy, loc, ammo );
            if( request.has_int( "qty" ) ) {
                opt.qty( request.get_int( "qty" ) );
            }
            if( !opt ) {
                return _( "can't reload that with this" );
            }
            guy.assign_activity( reload_activity_actor( std::move( opt ) ) );
            break;
        }
        case 'f':
            loc->is_favorite = !loc->is_favorite;
            if( loc.has_parent() && loc.parent_pocket() ) {
                loc.parent_pocket()->restack();
            }
            break;
        case '<':
        case '>':
            for( item_pocket *pocket : loc->get_all_standard_pockets() ) {
                pocket->settings.set_collapse( key == '>' );
            }
            break;
        default:
            return string_format( "unknown item action '%c'", static_cast<char>( key ) );
    }
    return std::string();
}

void send( const std::string &cmd, const std::function<void( JsonOut & )> &write )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", cmd );
    write( json );
    json.end_object();
    net::client_send_line( os.str() );
}

// avatar::invoke_item()'s question, without using the item here.
std::optional<std::string> choose_use_method( const item &used )
{
    const std::map<std::string, use_function> &use_methods = used.type->use_methods;
    if( use_methods.size() <= 1 ) {
        return use_methods.empty() ? std::string() : use_methods.begin()->first;
    }
    avatar &you = get_avatar();
    uilist umenu;
    umenu.text = string_format( _( "What to do with your %s?" ), used.tname() );
    umenu.hilight_disabled = true;
    std::vector<std::string> methods;
    for( const auto &e : use_methods ) {
        const auto res = e.second.can_call( you, used, you.pos_bub() );
        umenu.addentry_desc( methods.size(), res.success(), MENU_AUTOASSIGN, e.second.get_name(),
                             res.str() );
        methods.push_back( e.first );
    }
    umenu.desc_enabled = std::any_of( umenu.entries.begin(),
    umenu.entries.end(), []( const uilist_entry & elem ) {
        return !elem.desc.empty();
    } );
    umenu.query();
    if( umenu.ret < 0 || static_cast<size_t>( umenu.ret ) >= methods.size() ) {
        return std::nullopt;
    }
    return methods[umenu.ret];
}

void send_item( const int key, const item_location &loc, const std::optional<std::string> &method = std::nullopt,
                const std::optional<item::reload_option> &reload = std::nullopt )
{
    send( "item_action", [&]( JsonOut & json ) {
        json.member( "key", key );
        json.member( "item", loc );
        if( method && !method->empty() ) {
            json.member( "method", *method );
        }
        if( reload ) {
            json.member( "ammo", reload->ammo );
            json.member( "qty", reload->qty() );
        }
    } );
}

// Activates an item like avatar_action::use_item().
void use( const item_location &loc )
{
    if( !loc ) {
        return;
    }
    if( const std::optional<std::string> method = choose_use_method( *loc ) ) {
        send_item( 'a', loc, method );
    }
}

void reload( const item_location &loc, const bool prompt )
{
    if( !loc ) {
        return;
    }
    const item::reload_option opt = get_avatar().select_ammo( loc, prompt );
    if( opt ) {
        send_item( 'r', loc, std::nullopt, opt );
    }
}

} // namespace

std::string item_action( npc &guy, const JsonObject &request )
{
    item_location loc = read_location( request, "item" );
    if( !loc && request.has_array( "items" ) ) {
        return do_item_action( guy, loc, request.get_int( "key" ), request );
    }
    if( !loc ) {
        return _( "that item is gone" );
    }
    // The client's copy may be out of date or wrong: only the character's
    // own things and what lies close by.
    if( loc.carrier() != nullptr && loc.carrier() != &guy ) {
        return _( "that is not yours" );
    }
    if( loc.carrier() == nullptr && rl_dist( loc.pos_bub( get_map() ), guy.pos_bub() ) > 1 ) {
        return _( "that is too far away" );
    }
    return do_item_action( guy, loc, request.get_int( "key" ), request );
}

void set_client_active( const bool active )
{
    client_on = active;
}

bool client_active()
{
    return client_on;
}

bool uses_character( const action_id act )
{
    switch( act ) {
        case ACTION_PL_INFO:
        case ACTION_MEDICAL:
        case ACTION_BODYSTATUS:
        case ACTION_INVENTORY:
        case ACTION_WIELD:
        case ACTION_WEAR:
        case ACTION_TAKE_OFF:
        case ACTION_EAT:
        case ACTION_DROP:
        case ACTION_READ:
        case ACTION_USE:
        case ACTION_USE_WIELDED:
        case ACTION_RELOAD_ITEM:
        case ACTION_RELOAD_WEAPON:
        case ACTION_UNLOAD:
        case ACTION_MEND:
        case ACTION_DISASSEMBLE:
        case ACTION_COMPARE:
            return true;
        default:
            return false;
    }
}

bool load_character( const std::string &data )
{
    try {
        const JsonValue value = json_loader::from_string( data );
        // A fresh avatar each time: loading over the old copy keeps its id
        // and things the new data doesn't mention.
        get_avatar() = avatar();
        get_avatar().deserialize( value.get_object() );
    } catch( const std::exception &err ) {
        debugmsg( "Can't load the character from the host: %s", err.what() );
        return false;
    }
    return true;
}

void run( const action_id act )
{
    avatar &you = get_avatar();
    switch( act ) {
        case ACTION_PL_INFO:
            // Without customizing: changes would stay on this copy.
            you.disp_info( false );
            break;
        case ACTION_MEDICAL:
            you.disp_medical();
            break;
        case ACTION_BODYSTATUS:
            display_bodygraph( you );
            break;
        case ACTION_INVENTORY:
            game_menus::inv::common();
            break;
        // The same menus as in game::handle_action().
        case ACTION_WIELD:
            if( const item_location loc = game_menus::inv::wield() ) {
                send_item( 'w', loc );
            }
            break;
        case ACTION_WEAR:
            if( const item_location loc = game_menus::inv::wear( you ) ) {
                send_item( 'W', loc );
            }
            break;
        case ACTION_TAKE_OFF:
            if( const item_location loc = game_menus::inv::take_off() ) {
                send_item( 'T', loc );
            }
            break;
        case ACTION_EAT:
            if( const item_location loc = game_menus::inv::consume() ) {
                // avatar_action::eat_or_use()
                if( loc->is_comestible() || loc->is_medication() ) {
                    send_item( 'E', loc );
                } else {
                    use( loc );
                }
            }
            break;
        case ACTION_DROP: {
            const drop_locations what = game_menus::inv::multidrop( you );
            if( !what.empty() ) {
                send( "item_action", [&]( JsonOut & json ) {
                    json.member( "key", static_cast<int>( 'd' ) );
                    json.member( "items" );
                    json.start_array();
                    for( const drop_location &d : what ) {
                        json.start_array();
                        json.write( d.first );
                        json.write( d.second );
                        json.end_array();
                    }
                    json.end_array();
                } );
            }
            break;
        }
        case ACTION_READ:
            if( const item_location loc = game_menus::inv::read( you ) ) {
                send_item( 'R', loc );
            }
            break;
        case ACTION_USE:
            use( game_menus::inv::use() );
            break;
        case ACTION_USE_WIELDED:
            use( you.get_wielded_item() );
            break;
        case ACTION_RELOAD_ITEM:
            reload( game_menus::inv::titled_filter_menu( []( const item & it ) {
                return get_avatar().rate_action_reload( it ) == hint_rating::good;
            }, you, _( "Reload item" ), -1, _( "You have nothing to reload." ) ), false );
            break;
        case ACTION_RELOAD_WEAPON:
            reload( you.get_wielded_item(), false );
            break;
        case ACTION_UNLOAD: {
            const std::pair<item_location, bool> ret = game_menus::inv::unload( you );
            if( ret.first ) {
                send_item( 'U', ret.first );
            }
            break;
        }
        case ACTION_MEND:
            if( you.get_wielded_item() ) {
                send_item( 'm', you.get_wielded_item() );
            } else {
                add_msg( m_info, _( "You're not wielding anything." ) );
            }
            break;
        case ACTION_DISASSEMBLE:
            if( const item_location loc = game_menus::inv::disassemble( you ) ) {
                send_item( 'D', loc );
            }
            break;
        case ACTION_COMPARE:
            game_menus::inv::compare( std::nullopt );
            break;
        default:
            break;
    }
}

bool forward_item_action( const item_location &loc, const int key )
{
    if( !client_on || !loc ) {
        return false;
    }
    switch( key ) {
        case 'W':
        case 'w':
        case 'c':
        case 'T':
        case 'd':
        case 'U':
        case 'D':
        case 'm':
        case 'R':
            break;
        case 'E':
            if( loc->is_container() ) {
                // The host asks what to eat from the container.
                const item_location inside = game_menus::inv::consume( loc );
                if( !inside ) {
                    return true;
                }
                send_item( key, inside );
                return true;
            }
            break;
        case 'a':
            use( loc );
            return true;
        case 'r':
        case 'p':
            reload( loc, key == 'p' );
            return true;
        case 'f':
        case '<':
        case '>':
            // Done on both sides: the inventory stays open on this copy.
            send_item( key, loc );
            return false;
        case 't':
            popup( _( "Throwing is not available to the second player yet." ) );
            return true;
        default:
            // Only shows something (open, view recipe, ...): done here.
            return false;
    }
    send_item( key, loc );
    return true;
}

} // namespace mp::remote_actions
