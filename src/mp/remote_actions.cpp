#include "mp/remote_actions.h"

#include <algorithm>
#include <cmath>
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
#include "game.h"
#include "game_inventory.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "itype.h"
#include "character_martial_arts.h"
#include "messages.h"
#include "move_mode.h"
#include "json.h"
#include "json_loader.h"
#include "line.h"
#include "map.h"
#include "mp/net.h"
#include "npc.h"
#include "output.h"
#include "overmap_ui.h"
#include "projectile.h"
#include "ranged.h"
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
        case 'g': {
            drop_locations what;
            for( JsonArray entry : request.get_array( "items" ) ) {
                item_location it;
                it.deserialize( entry.get_object( 0 ) );
                if( it && it.carrier() == nullptr &&
                    rl_dist( it.pos_bub( get_map() ), guy.pos_bub() ) <= 1 ) {
                    what.emplace_back( it, entry.get_int( 1 ) );
                }
            }
            if( what.empty() ) {
                return _( "those things are gone" );
            }
            guy.pick_up( what );
            break;
        }
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

void send_setting( const std::string &what, const std::string &value )
{
    send( "setting", [&]( JsonOut & json ) {
        json.member( "what", what );
        json.member( "value", value );
    } );
}

void send_items( const int key, const drop_locations &what )
{
    if( what.empty() ) {
        return;
    }
    send( "item_action", [&]( JsonOut & json ) {
        json.member( "key", key );
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

std::string combat( npc &guy, const JsonObject &request )
{
    map &here = get_map();
    tripoint_abs_ms target_abs;
    request.read( "target", target_abs );
    const tripoint_bub_ms target = here.get_bub( target_abs );
    if( !here.inbounds( target ) ) {
        return _( "that target is too far away" );
    }
    const std::string action = request.get_string( "action", "" );
    if( action == "fire" ) {
        // aim_activity_actor::finish()
        item_location weapon = guy.get_wielded_item();
        if( !weapon || !weapon->is_gun() ) {
            return _( "you are not wielding a gun" );
        }
        const std::string mode = request.get_string( "mode", "" );
        if( !mode.empty() ) {
            weapon->gun_set_mode( gun_mode_id( mode ) );
        }
        // Aiming took no time of the host's: the client's aim counts.
        guy.recoil = request.get_float( "recoil", guy.recoil );
        gun_mode gun = weapon->gun_current_mode();
        if( !gun ) {
            return _( "that gun can't fire now" );
        }
        guy.fire_gun( here, target, gun.qty, *gun );
        return std::string();
    }
    if( action == "throw" ) {
        // avatar_action::plthrow()
        item_location loc = read_location( request, "item" );
        if( !loc || loc.carrier() != &guy ) {
            return _( "that is not yours" );
        }
        item *orig = loc.get_item();
        item thrown = *orig;
        if( guy.throw_range( thrown ) <= 0 ) {
            return _( "that is too heavy to throw" );
        }
        if( guy.is_worn( *orig ) && !guy.can_takeoff( *orig ).success() ) {
            return guy.can_takeoff( *orig ).str();
        }
        if( !guy.is_wielding( *orig ) && !guy.wield( *orig ) ) {
            return _( "you can't hold that to throw it" );
        }
        item_location weapon = guy.get_wielded_item();
        if( weapon->count_by_charges() && weapon->charges > 1 ) {
            weapon->mod_charges( -1 );
            thrown.charges = 1;
        } else {
            guy.remove_weapon();
        }
        guy.throw_item( target, thrown );
        return std::string();
    }
    return "unknown combat action \"" + action + "\"";
}

std::string activity( npc &guy, const JsonObject &request )
{
    player_activity act;
    act.deserialize( request.get_object( "data" ) );
    if( !act ) {
        return _( "nothing to do" );
    }
    guy.assign_activity( act );
    return std::string();
}

std::string construct( npc &guy, const JsonObject &request )
{
    tripoint_abs_ms where;
    request.read( "target", where );
    const tripoint_bub_ms p = get_map().get_bub( where );
    if( rl_dist( p, guy.pos_bub() ) > 1 ) {
        return _( "that is too far away" );
    }
    const construction_str_id id( request.get_string( "id", "" ) );
    if( !id.is_valid() ) {
        return _( "unknown construction" );
    }
    return construction_hooks::start( guy, id.id(), p );
}

std::string move_mode( npc &guy, const JsonObject &request )
{
    const move_mode_id mode( request.get_string( "mode", "" ) );
    if( !mode.is_valid() ) {
        return _( "unknown movement mode" );
    }
    if( !guy.can_switch_to( mode ) ) {
        return string_format( _( "you can't %s now" ), mode->name() );
    }
    guy.set_movement_mode( mode );
    return std::string();
}

std::string setting( npc &guy, const JsonObject &request )
{
    const std::string what = request.get_string( "what", "" );
    const std::string value = request.get_string( "value", "" );
    if( what == "style" ) {
        const matype_id style( value );
        if( !style.is_valid() ) {
            return _( "unknown style" );
        }
        guy.martial_arts_data->set_style( style );
        return std::string();
    }
    if( what == "fire_mode" ) {
        item_location weapon = guy.get_wielded_item();
        if( !weapon || !weapon->is_gun() ) {
            return _( "you are not wielding a gun" );
        }
        weapon->gun_set_mode( gun_mode_id( value ) );
        return std::string();
    }
    return "unknown setting \"" + what + "\"";
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
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL:
        case ACTION_FIRE:
        case ACTION_THROW:
        case ACTION_LOOK:
        case ACTION_MAP:
        case ACTION_OPEN_CONSUME:
        case ACTION_RELOAD_WIELDED:
        case ACTION_THROW_WIELDED:
        case ACTION_FIRE_BURST:
        case ACTION_SELECT_FIRE_MODE:
        case ACTION_PICK_STYLE:
        case ACTION_RECRAFT:
        case ACTION_LONGCRAFT:
            return true;
        default:
            return runs_host_code( act ) || changes_move_mode( act );
    }
}

bool runs_host_code( const action_id act )
{
    // The host's own code on the copy: what it does goes to the host as an
    // activity (forward_activity()) or a question answered here.
    switch( act ) {
        case ACTION_BUTCHER:
        case ACTION_ADVANCEDINV:
        case ACTION_SLEEP:
        case ACTION_WORKOUT:
        case ACTION_CAST_SPELL:
        case ACTION_UNLOAD_CONTAINER:
        case ACTION_INSERT_ITEM:
        case ACTION_DIR_DROP:
        case ACTION_CONSTRUCT:
        case ACTION_WAIT:
        case ACTION_RECAST_SPELL:
        // Only about the view or this client's settings.
        case ACTION_CENTER:
        case ACTION_SHIFT_N:
        case ACTION_SHIFT_NE:
        case ACTION_SHIFT_E:
        case ACTION_SHIFT_SE:
        case ACTION_SHIFT_S:
        case ACTION_SHIFT_SW:
        case ACTION_SHIFT_W:
        case ACTION_SHIFT_NW:
        case ACTION_TOGGLE_MAP_MEMORY:
        case ACTION_PEEK:
        case ACTION_LIST_ITEMS:
        case ACTION_MORALE:
        case ACTION_HELP:
        case ACTION_SKY:
        case ACTION_TOGGLE_SAFEMODE:
        case ACTION_TOGGLE_AUTOSAFE:
        case ACTION_TOGGLE_THIEF_MODE:
        case ACTION_TOGGLE_LANGUAGE_TO_EN:
        case ACTION_TOGGLE_AUTO_TRAVEL_MODE:
        case ACTION_IGNORE_ENEMY:
        case ACTION_WHITELIST_ENEMY:
            return true;
        default:
            return false;
    }
}

namespace
{

// handle_action.cpp, open_movement_mode_menu(), on the copy.
void movement_mode_menu( avatar &you )
{
    const std::vector<move_mode_id> &modes = move_modes_by_speed();
    const int cycle = 1027;
    uilist as_m;
    as_m.text = _( "Change to which movement mode?" );
    for( size_t i = 0; i < modes.size(); ++i ) {
        const move_mode_id &curr = modes[i];
        as_m.entries.emplace_back( static_cast<int>( i ), you.can_switch_to( curr ), curr->letter(),
                                   curr->name() );
    }
    as_m.entries.emplace_back( cycle, you.can_switch_to( you.current_movement_mode()->cycle() ),
                               hotkey_for_action( ACTION_OPEN_MOVEMENT, /*maximum_modifier_count=*/1 ),
                               _( "Cycle move mode" ) );
    as_m.selected = std::floor( modes.size() / 2 );
    as_m.query();
    if( as_m.ret != UILIST_CANCEL ) {
        if( as_m.ret == cycle ) {
            you.cycle_move_mode();
        } else if( as_m.ret >= 0 && static_cast<size_t>( as_m.ret ) < modes.size() ) {
            you.set_movement_mode( modes[as_m.ret] );
        }
    }
}

} // namespace

bool changes_move_mode( const action_id act )
{
    switch( act ) {
        case ACTION_TOGGLE_RUN:
        case ACTION_TOGGLE_CROUCH:
        case ACTION_TOGGLE_PRONE:
        case ACTION_CYCLE_MOVE:
        case ACTION_CYCLE_MOVE_REVERSE:
        case ACTION_OPEN_MOVEMENT:
        case ACTION_RESET_MOVE:
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
        // and things the new data doesn't mention. Where the view looks is
        // this client's own.
        const tripoint_rel_ms view_offset = get_avatar().view_offset;
        get_avatar() = avatar();
        get_avatar().deserialize( value.get_object() );
        get_avatar().view_offset = view_offset;
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
        case ACTION_OPEN_CONSUME:
            if( const item_location loc = game_menus::inv::consume() ) {
                // avatar_action::eat_or_use()
                if( loc->is_comestible() || loc->is_medication() ) {
                    send_item( 'E', loc );
                } else {
                    use( loc );
                }
            }
            break;
        case ACTION_DROP:
            send_items( 'd', game_menus::inv::multidrop( you ) );
            break;
        case ACTION_PICKUP:
            // game::pickup()
            if( const std::optional<tripoint_bub_ms> where = choose_adjacent( _( "Pick up items where?" ) ) ) {
                send_items( 'g', game_menus::inv::pickup( where ) );
            }
            break;
        case ACTION_PICKUP_ALL:
            send_items( 'g', game_menus::inv::pickup() );
            break;
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
        case ACTION_RELOAD_WIELDED:
            reload( you.get_wielded_item(), false );
            break;
        case ACTION_SELECT_FIRE_MODE: {
            item_location weapon = you.get_wielded_item();
            if( weapon && weapon->is_gun() && !weapon->is_gunmod() ) {
                if( weapon->gun_all_modes().size() > 1 ) {
                    weapon->gun_cycle_mode();
                    send_setting( "fire_mode", weapon->gun_get_mode_id().str() );
                } else {
                    add_msg( m_info, _( "Your %s has only one firing mode." ), weapon->tname() );
                }
            }
            break;
        }
        case ACTION_PICK_STYLE:
            if( you.martial_arts_data->pick_style( you ) ) {
                send_setting( "style", you.martial_arts_data->selected_style().str() );
            }
            break;
        case ACTION_RECRAFT:
        case ACTION_LONGCRAFT:
            if( !you.lastrecipe.is_valid() || you.lastrecipe.is_null() ) {
                popup( _( "Craft something first" ) );
            } else {
                send( "craft", [&]( JsonOut & json ) {
                    json.member( "recipe", you.lastrecipe.str() );
                    json.member( "batch", std::max( 1, you.last_batch ) );
                } );
            }
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
        case ACTION_LOOK:
            g->look_around();
            break;
        case ACTION_TOGGLE_RUN:
        case ACTION_TOGGLE_CROUCH:
        case ACTION_TOGGLE_PRONE:
        case ACTION_CYCLE_MOVE:
        case ACTION_CYCLE_MOVE_REVERSE:
        case ACTION_RESET_MOVE:
        case ACTION_OPEN_MOVEMENT: {
            // The avatar's own toggles on the copy, then the result to the host.
            const move_mode_id before = you.current_movement_mode();
            if( act == ACTION_TOGGLE_RUN ) {
                you.toggle_run_mode();
            } else if( act == ACTION_TOGGLE_CROUCH ) {
                you.toggle_crouch_mode();
            } else if( act == ACTION_TOGGLE_PRONE ) {
                you.toggle_prone_mode();
            } else if( act == ACTION_CYCLE_MOVE ) {
                you.cycle_move_mode();
            } else if( act == ACTION_CYCLE_MOVE_REVERSE ) {
                you.cycle_move_mode_reverse();
            } else if( act == ACTION_RESET_MOVE ) {
                you.reset_move_mode();
            } else {
                movement_mode_menu( you );
            }
            if( you.current_movement_mode() != before ) {
                send( "move_mode", [&]( JsonOut & json ) {
                    json.member( "mode", you.current_movement_mode().str() );
                } );
            }
            break;
        }
        default:
            if( runs_host_code( act ) ) {
                g->do_action_for_mirror( act );
            }
            break;
        case ACTION_MAP:
            ui::omap::display();
            break;
        case ACTION_FIRE_BURST:
        case ACTION_FIRE: {
            item_location weapon = you.get_wielded_item();
            if( act == ACTION_FIRE_BURST && weapon && weapon->is_gun() &&
                !weapon->gun_set_mode( gun_mode_id( "BURST" ) ) && !weapon->gun_set_mode( gun_mode_id( "AUTO" ) ) ) {
                break;
            }
            if( !weapon || !weapon->is_gun() ) {
                add_msg( m_info, _( "You are not wielding a ranged weapon." ) );
                break;
            }
            // The host's aiming screen (aim_activity_actor::do_turn()).
            aim_activity_actor aim = aim_activity_actor::use_wielded();
            target_handler::trajectory trajectory;
            // Aiming ("aim and fire", '.') spends moves over several turns: the
            // host's activity reopens the screen each turn. Time doesn't count
            // for the second player, so it goes on at once.
            for( int turns = 0; turns < 100 && trajectory.empty() && !aim.aborted; turns++ ) {
                you.set_moves( you.get_speed() );
                trajectory = target_handler::mode_fire( you, aim );
                if( aim.action.empty() ) {
                    break;
                }
            }
            if( trajectory.empty() ) {
                break;
            }
            send( "combat", [&]( JsonOut & json ) {
                json.member( "action", "fire" );
                json.member( "target", get_map().get_abs( trajectory.back() ) );
                json.member( "recoil", you.recoil );
                json.member( "mode", weapon->gun_get_mode_id().str() );
            } );
            break;
        }
        case ACTION_THROW_WIELDED:
        case ACTION_THROW: {
            item_location loc = act == ACTION_THROW_WIELDED ? you.get_wielded_item() :
                                game_menus::inv::titled_menu( you, _( "Throw item" ),
                                        _( "You don't have any items to throw." ) );
            if( !loc ) {
                break;
            }
            const target_handler::trajectory trajectory = target_handler::mode_throw( you, *loc, false );
            if( trajectory.empty() ) {
                break;
            }
            send( "combat", [&]( JsonOut & json ) {
                json.member( "action", "throw" );
                json.member( "item", loc );
                json.member( "target", get_map().get_abs( trajectory.back() ) );
            } );
            break;
        }
    }
}

bool forward_activity( const Character &who, const player_activity &act )
{
    if( !client_on || &who != &get_avatar() || !act ) {
        return false;
    }
    send( "activity", [&]( JsonOut & json ) {
        json.member( "data", act );
    } );
    return true;
}

bool forward_construction( const construction_id &id, const tripoint_abs_ms &where )
{
    if( !client_on ) {
        return false;
    }
    send( "construct", [&]( JsonOut & json ) {
        json.member( "id", id.id().str() );
        json.member( "target", where );
    } );
    return true;
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
        case 't': {
            // avatar_action::plthrow() from the item menu.
            item_location thrown = loc;
            const target_handler::trajectory trajectory = target_handler::mode_throw( get_avatar(), *thrown,
                    false );
            if( !trajectory.empty() ) {
                send( "combat", [&]( JsonOut & json ) {
                    json.member( "action", "throw" );
                    json.member( "item", loc );
                    json.member( "target", get_map().get_abs( trajectory.back() ) );
                } );
            }
            return true;
        }
        default:
            // Only shows something (open, view recipe, ...): done here.
            return false;
    }
    send_item( key, loc );
    return true;
}

} // namespace mp::remote_actions
