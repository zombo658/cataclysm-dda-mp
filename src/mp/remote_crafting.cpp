#include "mp/remote_crafting.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catacharset.h"
#include "color.h"
#include "crafting_gui.h"
#include "cursesdef.h"
#include "input_context.h"
#include "json.h"
#include "localized_comparator.h"
#include "mp/net.h"
#include "npc.h"
#include "output.h"
#include "point.h"
#include "popup.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "string_input_popup.h"
#include "translations.h"
#include "type_id.h"
#include "ui_iteminfo.h"
#include "ui_manager.h"
#include "uilist.h"
#include "uistate.h"

namespace mp::remote_crafting
{

namespace
{

// ---- Host ----

// The recipes guy may use now (known, from books nearby, or known by
// helpers), without the kinds the second player can't start yet.
std::map<std::string, const recipe *> usable_recipes( npc &guy )
{
    std::map<std::string, const recipe *> result;
    for( const recipe *r : guy.get_group_available_recipes() ) {
        if( r->obsolete || r->is_blueprint() || r->is_practice() || r->is_nested() ) {
            continue;
        }
        result.emplace( r->ident().str(), r );
    }
    return result;
}

const recipe *find( const std::map<std::string, const recipe *> &recipes, const std::string &id )
{
    const auto it = recipes.find( id );
    return it == recipes.end() ? nullptr : it->second;
}

} // namespace

void write_recipes( JsonOut &json, npc &guy )
{
    json.member( "ids" );
    json.start_array();
    for( const auto &entry : usable_recipes( guy ) ) {
        json.write( entry.first );
    }
    json.end_array();
}

void write_states( JsonOut &json, npc &guy, const JsonObject &request )
{
    const std::map<std::string, const recipe *> recipes = usable_recipes( guy );
    json.member( "items" );
    json.start_array();
    for( JsonArray item : request.get_array( "items" ) ) {
        const std::string id = item.get_string( 0 );
        const int batch = std::max( 1, item.get_int( 1 ) );
        craft_screen::recipe_state state;
        if( const recipe *r = find( recipes, id ) ) {
            state = craft_screen::state( guy, *r, batch );
        }
        json.start_array();
        json.write( id );
        json.write( batch );
        json.write( state.can_craft );
        json.write( state.has_primary_skill );
        json.write( string_from_color( state.color ) );
        json.write( string_from_color( state.selected_color ) );
        json.write( string_from_color( state.info_color ) );
        json.end_array();
    }
    json.end_array();
}

void write_info( JsonOut &json, npc &guy, const JsonObject &request )
{
    const std::string id = request.get_string( "recipe" );
    const int batch = std::max( 1, request.get_int( "batch", 1 ) );
    const int width = std::clamp( request.get_int( "width", 40 ), 10, 500 );
    const int result_width = std::clamp( request.get_int( "result_width", 0 ), 0, 500 );
    json.member( "recipe", id );
    json.member( "batch", batch );
    json.member( "width", width );
    json.member( "result_width", result_width );
    const recipe *r = find( usable_recipes( guy ), id );
    if( r == nullptr ) {
        json.member( "lines", std::vector<std::string> { colorize( _( "You can't use this recipe now." ), c_red ) } );
        return;
    }
    json.member( "lines", craft_screen::info( guy, *r, batch, width ) );
    const std::pair<nc_color, std::string> indicator = craft_screen::speed_indicator( guy, *r );
    json.member( "indicator_color", string_from_color( indicator.first ) );
    json.member( "indicator", indicator.second );
    if( result_width > 0 ) {
        json.member( "result", craft_screen::result_info( guy, *r, batch, result_width ) );
    }
}

void write_filter( JsonOut &json, npc &guy, const JsonObject &request )
{
    const std::string query = request.get_string( "filter", "" );
    recipe_subset recipes;
    for( const auto &entry : usable_recipes( guy ) ) {
        recipes.include( entry.second );
    }
    json.member( "filter", query );
    json.member( "ids" );
    json.start_array();
    for( const recipe *r : craft_screen::filter( recipes, query, guy ) ) {
        json.write( r->ident().str() );
    }
    json.end_array();
}

std::string start( npc &guy, const std::string &id, const int batch )
{
    const recipe *r = find( usable_recipes( guy ), id );
    if( r == nullptr ) {
        return _( "you can't use that recipe now" );
    }
    if( batch < 1 ) {
        return _( "bad batch size" );
    }
    if( !guy.can_start_craft( r, recipe_filter_flags::none, batch ) ) {
        return _( "you don't have everything you need" );
    }
    if( guy.lighting_craft_speed_multiplier( *r ) <= 0.0f ) {
        return _( "it's too dark to craft" );
    }
    guy.make_craft_with_command( r->ident(), batch, false, std::nullopt );
    if( !guy.activity ) {
        return _( "the craft didn't start" );
    }
    return std::string();
}

// ---- Client ----

bool read_message( client_cache &cache, const std::string &type, const JsonObject &message )
{
    if( type == "recipes" ) {
        std::vector<std::string> ids;
        for( const std::string id : message.get_array( "ids" ) ) {
            ids.push_back( id );
        }
        cache.recipes = ids;
    } else if( type == "recipe_states" ) {
        for( JsonArray item : message.get_array( "items" ) ) {
            craft_screen::recipe_state state;
            const std::string id = item.get_string( 0 );
            const int batch = item.get_int( 1 );
            state.can_craft = item[2].get_bool();
            state.has_primary_skill = item[3].get_bool();
            state.color = color_from_string( item.get_string( 4 ) );
            state.selected_color = color_from_string( item.get_string( 5 ) );
            state.info_color = color_from_string( item.get_string( 6 ) );
            cache.states[ { id, batch }] = state;
        }
    } else if( type == "recipe_info" ) {
        const auto key = std::make_tuple( message.get_string( "recipe" ), message.get_int( "batch" ),
                                          message.get_int( "width" ), message.get_int( "result_width" ) );
        description d;
        for( const std::string line : message.get_array( "lines" ) ) {
            d.lines.push_back( line );
        }
        d.indicator_color = color_from_string( message.get_string( "indicator_color", "c_white" ) );
        d.indicator = message.get_string( "indicator", "" );
        d.result = message.get_string( "result", "" );
        cache.infos[key] = d;
        cache.infos_asked.erase( key );
    } else if( type == "recipe_filter" ) {
        std::vector<std::string> ids;
        for( const std::string id : message.get_array( "ids" ) ) {
            ids.push_back( id );
        }
        cache.filters[message.get_string( "filter" )] = ids;
    } else {
        return false;
    }
    return true;
}

namespace
{

template<typename Writer>
void send( const std::string &cmd, const Writer &write )
{
    std::ostringstream os;
    JsonOut json( os );
    json.start_object();
    json.member( "cmd", cmd );
    write( json );
    json.end_object();
    net::client_send_line( os.str() );
}

// Waits for an answer of the host, as the host's screen waits for its own
// computations. False if the connection is lost or nothing came.
bool wait_for( const std::function<bool()> &poll, const std::function<bool()> &arrived )
{
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while( !arrived() ) {
        if( !poll() || std::chrono::steady_clock::now() > give_up ) {
            return false;
        }
        // Keys pressed meanwhile stay queued, as on the host's screen.
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return true;
}

// Asks for the states of these (recipe, batch) pairs the cache doesn't have.
bool fetch_states( client_cache &cache, const std::function<bool()> &poll,
                   const std::vector<std::pair<std::string, int>> &wanted )
{
    std::vector<std::pair<std::string, int>> missing;
    for( const auto &w : wanted ) {
        if( !cache.states.count( w ) ) {
            missing.push_back( w );
        }
    }
    if( missing.empty() ) {
        return true;
    }
    send( "recipe_states", [&]( JsonOut & json ) {
        json.member( "items" );
        json.start_array();
        for( const auto &m : missing ) {
            json.start_array();
            json.write( m.first );
            json.write( m.second );
            json.end_array();
        }
        json.end_array();
    } );
    return wait_for( poll, [&]() {
        return std::all_of( missing.begin(), missing.end(), [&]( const auto & m ) {
            return cache.states.count( m ) > 0;
        } );
    } );
}

craft_screen::recipe_state state_of( const client_cache &cache, const recipe *r, const int batch )
{
    const auto it = cache.states.find( { r->ident().str(), batch } );
    return it == cache.states.end() ? craft_screen::recipe_state() : it->second;
}

} // namespace

// A copy of select_crafter_and_crafting_recipe() (crafting_gui.cpp): the same
// windows, tabs, keys and texts, with the character's side asked from the
// host. Not here yet: nested recipe groups, practice, related recipes,
// comparing, choosing the crafter, read/unread marks.
void show_screen( client_cache &cache, const std::function<bool()> &poll )
{
    cache.states.clear();
    cache.infos.clear();
    cache.infos_asked.clear();
    cache.filters.clear();
    cache.recipes.reset();
    send( "recipes", []( JsonOut & ) {} );
    if( !wait_for( poll, [&]() {
    return cache.recipes.has_value();
    } ) ) {
        return;
    }
    recipe_subset available_recipes;
    for( const std::string &id : *cache.recipes ) {
        const recipe_id rid( id );
        // The client has the host's data, but be careful with mismatched mods.
        if( rid.is_valid() ) {
            available_recipes.include( &rid.obj() );
        }
    }

    int line_recipe_info = 0;
    int line_item_info = 0;
    const int headHeight = 3;
    const int subHeadHeight = 2;

    bool isWide = false;
    int width = 0;
    int dataLines = 0;
    int dataHeight = 0;
    int item_info_width = 0;

    input_context ctxt( "CRAFTING" );
    ctxt.register_cardinal();
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "SCROLL_RECIPE_INFO_UP" );
    ctxt.register_action( "SCROLL_RECIPE_INFO_DOWN" );
    ctxt.register_action( "PAGE_UP", to_translation( "Fast scroll up" ) );
    ctxt.register_action( "PAGE_DOWN", to_translation( "Fast scroll down" ) );
    ctxt.register_action( "HOME" );
    ctxt.register_action( "END" );
    ctxt.register_action( "SCROLL_ITEM_INFO_UP" );
    ctxt.register_action( "SCROLL_ITEM_INFO_DOWN" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "TOGGLE_FAVORITE" );
    ctxt.register_action( "HELP_RECIPE" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "CYCLE_BATCH" );
    ctxt.register_action( "HIDE_SHOW_RECIPE" );
    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );
    ctxt.set_timeout( 50 );

    catacurses::window w_head_tabs;
    catacurses::window w_head_info;
    catacurses::window w_subhead;
    catacurses::window w_data;
    catacurses::window w_iteminfo;
    std::vector<std::string> keybinding_tips;
    int keybinding_x = 0;
    ui_adaptor ui;
    ui.on_screen_resize( [&]( ui_adaptor & ui ) {
        const int freeWidth = TERMX - FULL_SCREEN_WIDTH;
        isWide = ( TERMX > FULL_SCREEN_WIDTH && freeWidth > 15 );

        width = isWide ? ( freeWidth > FULL_SCREEN_WIDTH ? FULL_SCREEN_WIDTH * 2 : TERMX ) :
                FULL_SCREEN_WIDTH;
        const unsigned int header_info_width = std::max( width / 4, width - FULL_SCREEN_WIDTH - 1 );
        const int wStart = ( TERMX - width ) / 2;

        std::vector<std::string> act_descs;
        const auto add_action_desc = [&]( const std::string & act, const std::string & txt ) {
            act_descs.emplace_back(
                ctxt.get_desc( act, txt, input_context::allow_all_keys ) );
        };
        add_action_desc( "CONFIRM", pgettext( "crafting gui", "Craft" ) );
        add_action_desc( "HELP_RECIPE", pgettext( "crafting gui", "Describe" ) );
        add_action_desc( "FILTER", pgettext( "crafting gui", "Filter" ) );
        add_action_desc( "RESET_FILTER", pgettext( "crafting gui", "Reset filter" ) );
        add_action_desc( "HIDE_SHOW_RECIPE", pgettext( "crafting gui", "Show/hide" ) );
        add_action_desc( "TOGGLE_FAVORITE", pgettext( "crafting gui", "Favorite" ) );
        add_action_desc( "CYCLE_BATCH", pgettext( "crafting gui", "Batch" ) );
        add_action_desc( "HELP_KEYBINDINGS", pgettext( "crafting gui", "Keybindings" ) );
        keybinding_x = isWide ? 5 : 2;
        keybinding_tips = foldstring( enumerate_as_string( act_descs, enumeration_conjunction::none ),
                                      width - keybinding_x * 2 );

        const int tailHeight = keybinding_tips.size() + 2;
        dataLines = TERMY - ( headHeight + subHeadHeight ) - tailHeight;
        dataHeight = TERMY - ( headHeight + subHeadHeight );

        w_head_tabs = catacurses::newwin( headHeight, ( width - header_info_width ), point( wStart, 0 ) );
        w_head_info = catacurses::newwin( headHeight, header_info_width,
                                          point( wStart + ( width - header_info_width ), 0 ) );
        w_subhead = catacurses::newwin( subHeadHeight, width, point( wStart, 3 ) );
        w_data = catacurses::newwin( dataHeight, width, point( wStart,
                                     headHeight + subHeadHeight ) );

        if( isWide ) {
            item_info_width = width - FULL_SCREEN_WIDTH - 1;
            const int item_info_height = dataHeight - tailHeight;
            const point item_info( wStart + width - item_info_width, headHeight + subHeadHeight );
            w_iteminfo = catacurses::newwin( item_info_height, item_info_width, item_info );
        } else {
            item_info_width = 0;
            w_iteminfo = {};
        }

        ui.position( point( wStart, 0 ), point( width, TERMY ) );
    } );
    ui.mark_resize();

    const std::vector<std::string> crafting_categories = craft_screen::categories();
    if( crafting_categories.empty() ) {
        popup( _( "The host's game data isn't loaded." ) );
        return;
    }
    tab_list tab( crafting_categories );
    tab_list subtab( crafting_category_id( tab.cur() )->subcategories );
    std::vector<const recipe *> current;
    int line = 0;
    bool recalc = true;
    bool keepline = false;
    bool done = false;
    bool batch = false;
    bool show_hidden = false;
    size_t num_hidden = 0;
    int num_recipe = 0;
    int batch_line = 0;
    const recipe *chosen = nullptr;
    std::string filterstring;
    const int max_recipe_name_width = 27;
    // border + padding + name + padding
    const int xpos = 1 + 1 + max_recipe_name_width + 3;
    const int fold_width = FULL_SCREEN_WIDTH - xpos - 2;

    // The description of the current line, if the host has sent it already.
    const auto current_info = [&]() -> const description * {
        if( current.empty() )
        {
            return nullptr;
        }
        const auto key = std::make_tuple( current[line]->ident().str(), batch ? line + 1 : 1,
                                          fold_width, isWide ? item_info_width : 0 );
        const auto it = cache.infos.find( key );
        if( it != cache.infos.end() )
        {
            return &it->second;
        }
        if( !cache.infos_asked.count( key ) )
        {
            cache.infos_asked.insert( key );
            send( "recipe_info", [&]( JsonOut & json ) {
                json.member( "recipe", std::get<0>( key ) );
                json.member( "batch", std::get<1>( key ) );
                json.member( "width", std::get<2>( key ) );
                json.member( "result_width", std::get<3>( key ) );
            } );
        }
        return nullptr;
    };

    ui.on_redraw( [&]( ui_adaptor & ui ) {
        // The tabs, as draw_recipe_tabs() and draw_recipe_subtabs() draw them.
        werase( w_head_tabs );
        if( batch || !filterstring.empty() ) {
            wattron( w_head_tabs, BORDER_COLOR );
            mvwhline( w_head_tabs, point( 0, getmaxy( w_head_tabs ) - 1 ), LINE_OXOX,
                      getmaxx( w_head_tabs ) - 1 );
            mvwaddch( w_head_tabs, point( 0, getmaxy( w_head_tabs ) - 1 ), LINE_OXXO );
            wattroff( w_head_tabs, BORDER_COLOR );
            draw_tab( w_head_tabs, 2, batch ? _( "Batch" ) : _( "Searched" ), true );
        } else {
            std::vector<std::string> translated_cats;
            for( const std::string &cat : crafting_categories ) {
                translated_cats.emplace_back( _( craft_screen::category_name( cat ) ) );
            }
            const std::pair<std::vector<std::string>, size_t> fitted_tabs = fit_tabs_to_width(
                        getmaxx( w_head_tabs ), tab.cur_index(), translated_cats );
            draw_tabs( w_head_tabs, fitted_tabs.first, tab.cur_index() - fitted_tabs.second,
                       fitted_tabs.second );
        }
        mvwputch( w_head_tabs, point( getmaxx( w_head_tabs ) - 1, 2 ), BORDER_COLOR, LINE_OXOX );
        wnoutrefresh( w_head_tabs );

        werase( w_subhead );
        const int sub_width = getmaxx( w_subhead );
        wattron( w_subhead, BORDER_COLOR );
        mvwvline( w_subhead, point::zero, LINE_XOXO, getmaxy( w_subhead ) );
        mvwvline( w_subhead, point( sub_width - 1, 0 ), LINE_XOXO, getmaxy( w_subhead ) );
        wattroff( w_subhead, BORDER_COLOR );
        if( !batch && filterstring.empty() ) {
            const crafting_category_id current_cat( tab.cur() );
            std::vector<std::string> translated_subcats;
            std::vector<bool> empty_subcats;
            for( const std::string &subcat : current_cat->subcategories ) {
                translated_subcats.emplace_back( _( craft_screen::subcategory_name( tab.cur(), subcat ) ) );
                empty_subcats.emplace_back( available_recipes.empty_category( current_cat,
                                            subcat != "CSC_ALL" ? subcat : "" ) );
            }
            const std::pair<std::vector<std::string>, size_t> fitted = fit_tabs_to_width( sub_width,
                    subtab.cur_index(), translated_subcats );
            const size_t offset = fitted.second;
            if( fitted.first.size() + offset <= translated_subcats.size() ) {
                int pos_x = 2;
                for( size_t i = 0; i < fitted.first.size(); ++i ) {
                    draw_subtab( w_subhead, pos_x, fitted.first[i],
                                 static_cast<size_t>( subtab.cur_index() ) == i + offset, true,
                                 empty_subcats[i + offset] );
                    pos_x += utf8_width( fitted.first[i] ) + 3;
                }
            }
        }
        wnoutrefresh( w_subhead );

        werase( w_head_info );
        if( !show_hidden ) {
            // draw_hidden_amount()
            if( num_hidden == 1 ) {
                right_print( w_head_info, 1, 1, c_red,
                             string_format( _( "* %s hidden recipe - %s in category *" ), num_hidden, num_recipe ) );
            } else if( num_hidden >= 2 ) {
                right_print( w_head_info, 1, 1, c_red,
                             string_format( _( "* %s hidden recipes - %s in category *" ), num_hidden, num_recipe ) );
            } else {
                right_print( w_head_info, 1, 1, c_green,
                             string_format( _( "* No hidden recipe - %s in category *" ), num_recipe ) );
            }
            wattron( w_head_info, BORDER_COLOR );
            mvwhline( w_head_info, point( 0, getmaxy( w_head_info ) - 1 ), LINE_OXOX,
                      getmaxx( w_head_info ) - 1 );
            mvwaddch( w_head_info, point( getmaxx( w_head_info ) - 1, getmaxy( w_head_info ) - 1 ),
                      LINE_OOXX );
            wattroff( w_head_info, BORDER_COLOR );
        }

        werase( w_data );
        for( size_t i = 0; i < keybinding_tips.size(); ++i ) {
            nc_color dummy = c_white;
            print_colored_text( w_data, point( keybinding_x, dataLines + 1 + i ),
                                dummy, c_white, keybinding_tips[i] );
        }
        wattron( w_data, BORDER_COLOR );
        mvwhline( w_data, point( 1, dataHeight - 1 ), LINE_OXOX, width - 2 );
        mvwvline( w_data, point::zero, LINE_XOXO, dataHeight - 1 );
        mvwvline( w_data, point( width - 1, 0 ), LINE_XOXO, dataHeight - 1 );
        mvwaddch( w_data, point( 0, dataHeight - 1 ), LINE_XXOO );
        mvwaddch( w_data, point( width - 1, dataHeight - 1 ), LINE_XOOX );
        wattroff( w_data, BORDER_COLOR );

        const int recmax = current.size();
        const auto& [istart, iend] = subindex_around_cursor( recmax, dataLines, line );
        for( int i = istart; i < iend; ++i ) {
            std::string tmp_name = current[i]->result_name( /*decorated=*/true );
            if( batch ) {
                tmp_name = string_format( _( "%2dx %s" ), i + 1, tmp_name );
            }
            const craft_screen::recipe_state st = state_of( cache, current[i], batch ? i + 1 : 1 );
            const bool highlight = i == line;
            const point print_from( 2, i - istart );
            if( highlight ) {
                ui.set_cursor( w_data, print_from );
            }
            mvwprintz( w_data, print_from, highlight ? st.selected_color : st.color, "%s",
                       trim_by_length( tmp_name, max_recipe_name_width ) );
        }

        if( !current.empty() ) {
            const craft_screen::recipe_state st = state_of( cache, current[line], batch ? line + 1 : 1 );
            const description *info = current_info();
            if( info != nullptr ) {
                right_print( w_head_info, 0, 1, info->indicator_color, info->indicator );
                const int total_lines = info->lines.size();
                line_recipe_info = clamp( line_recipe_info, 0, total_lines - dataLines );
                for( int i = line_recipe_info; i < std::min( line_recipe_info + dataLines, total_lines ); ++i ) {
                    nc_color dummy = st.info_color;
                    print_colored_text( w_data, point( xpos, i - line_recipe_info ), dummy, st.info_color,
                                        info->lines[i] );
                }
                if( total_lines > dataLines ) {
                    scrollbar()
                    .offset_x( xpos + fold_width + 1 )
                    .content_size( total_lines )
                    .viewport_pos( line_recipe_info )
                    .viewport_size( dataLines )
                    .apply( w_data );
                }
            } else {
                mvwprintz( w_data, point( xpos, 0 ), c_dark_gray, "%s", _( "Asking the host…" ) );
            }
        }
        wnoutrefresh( w_head_info );

        scrollbar()
        .offset_x( 0 )
        .offset_y( 0 )
        .content_size( recmax )
        .viewport_pos( istart )
        .viewport_size( dataLines )
        .apply( w_data );
        wnoutrefresh( w_data );

        if( isWide && !current.empty() ) {
            werase( w_iteminfo );
            const description *info = current_info();
            if( info != nullptr ) {
                item_info_data data( "", "", { iteminfo( "DESCRIPTION", info->result ) }, {},
                                     line_item_info );
                data.without_getch = true;
                data.without_border = true;
                data.scrollbar_left = false;
                data.use_full_win = true;
                data.padding = 0;
                draw_item_info( w_iteminfo, data );
            }
            wnoutrefresh( w_iteminfo );
        }
    } );

    // How many descriptions had arrived at the last redraw.
    size_t infos_seen = cache.infos.size();
    do {
        if( recalc ) {
            recalc = false;
            const recipe *prev_rcp = nullptr;
            if( keepline && line >= 0 && static_cast<size_t>( line ) < current.size() ) {
                prev_rcp = current[line];
            }
            show_hidden = false;

            if( batch ) {
                current.assign( 50, chosen );
                std::vector<std::pair<std::string, int>> wanted;
                for( int i = 1; i <= 50; i++ ) {
                    wanted.emplace_back( chosen->ident().str(), i );
                }
                if( !fetch_states( cache, poll, wanted ) ) {
                    return;
                }
            } else {
                std::vector<const recipe *> picking;
                if( !filterstring.empty() ) {
                    if( !cache.filters.count( filterstring ) ) {
                        static_popup searching;
                        searching.message( "%s", _( "Searching…" ) );
                        ui_manager::redraw();
                        send( "recipe_filter", [&]( JsonOut & json ) {
                            json.member( "filter", filterstring );
                        } );
                        if( !wait_for( poll, [&]() {
                        return cache.filters.count( filterstring ) > 0;
                        } ) ) {
                            return;
                        }
                    }
                    for( const std::string &id : cache.filters[filterstring] ) {
                        const recipe_id rid( id );
                        if( rid.is_valid() ) {
                            picking.push_back( &rid.obj() );
                        }
                    }
                } else {
                    const std::pair<std::vector<const recipe *>, bool> result = recipes_from_cat(
                                available_recipes, crafting_category_id( tab.cur() ), subtab.cur() );
                    show_hidden = result.second;
                    if( show_hidden ) {
                        current = result.first;
                    } else {
                        picking = result.first;
                    }
                }

                if( !show_hidden ) {
                    current.clear();
                    for( const recipe *i : picking ) {
                        if( uistate.hidden_recipes.find( i->ident() ) == uistate.hidden_recipes.end() ) {
                            current.push_back( i );
                        }
                    }
                    num_hidden = picking.size() - current.size();
                    num_recipe = picking.size();
                }

                std::vector<std::pair<std::string, int>> wanted;
                for( const recipe *r : current ) {
                    wanted.emplace_back( r->ident().str(), 1 );
                }
                if( !fetch_states( cache, poll, wanted ) ) {
                    return;
                }

                if( subtab.cur() != "CSC_*_RECENT" ) {
                    std::stable_sort( current.begin(), current.end(), [&]( const recipe * const a,
                    const recipe * const b ) {
                        const bool can_craft_a = state_of( cache, a, 1 ).can_craft;
                        const bool can_craft_b = state_of( cache, b, 1 ).can_craft;
                        if( can_craft_a != can_craft_b ) {
                            return can_craft_a;
                        }
                        if( b->difficulty != a->difficulty ) {
                            return b->difficulty < a->difficulty;
                        }
                        return localized_compare( a->result_name(), b->result_name() );
                    } );
                }
            }

            line = 0;
            if( keepline && prev_rcp ) {
                for( size_t i = 0; i < current.size(); i++ ) {
                    if( current[i] == prev_rcp ) {
                        line = i;
                        break;
                    }
                }
            }
        }
        keepline = false;

        ui_manager::redraw();
        const int scroll_item_info_lines = catacurses::getmaxy( w_iteminfo ) - 4;
        const std::string action = ctxt.handle_input();
        if( action == "TIMEOUT" ) {
            if( !poll() ) {
                return;
            }
            if( cache.infos.size() != infos_seen ) {
                infos_seen = cache.infos.size();
                ui.invalidate_ui();
            }
            continue;
        }
        const int recmax = static_cast<int>( current.size() );
        const int scroll_rate = recmax > 20 ? 10 : 3;
        if( action == "SCROLL_RECIPE_INFO_UP" ) {
            line_recipe_info -= dataLines;
        } else if( action == "SCROLL_RECIPE_INFO_DOWN" ) {
            line_recipe_info += dataLines;
        } else if( action == "LEFT" || action == "RIGHT" ) {
            if( batch || !filterstring.empty() ) {
                continue;
            }
            const std::string start = subtab.cur();
            do {
                if( action == "LEFT" ) {
                    subtab.prev();
                } else {
                    subtab.next();
                }
            } while( subtab.cur() != start &&
                     available_recipes.empty_category( crafting_category_id( tab.cur() ),
                             subtab.cur() != "CSC_ALL" ? subtab.cur() : "" ) );
            recalc = true;
        } else if( action == "SCROLL_ITEM_INFO_UP" ) {
            line_item_info -= scroll_item_info_lines;
        } else if( action == "SCROLL_ITEM_INFO_DOWN" ) {
            line_item_info += scroll_item_info_lines;
        } else if( action == "PREV_TAB" || action == "NEXT_TAB" ) {
            if( action == "PREV_TAB" ) {
                tab.prev();
            } else {
                tab.next();
            }
            subtab = tab_list( crafting_category_id( tab.cur() )->subcategories );
            recalc = true;
        } else if( action == "DOWN" || ( action == "SCROLL_DOWN" && recmax > 0 ) ) {
            line++;
        } else if( action == "UP" || ( action == "SCROLL_UP" && recmax > 0 ) ) {
            line--;
        } else if( action == "PAGE_UP" || action == "PAGE_DOWN" ) {
            line = inc_clamp( line, action == "PAGE_UP" ? -scroll_rate : scroll_rate, recmax );
        } else if( action == "HOME" ) {
            line = 0;
        } else if( action == "END" ) {
            line = -1;
        } else if( action == "CONFIRM" ) {
            if( current.empty() ) {
                popup( _( "Nothing selected!" ) );
            } else {
                const craft_screen::recipe_state st = state_of( cache, current[line], batch ? line + 1 : 1 );
                if( !st.can_craft || !st.has_primary_skill ) {
                    popup( _( "Crafter can't craft that!" ) );
                } else {
                    chosen = current[line];
                    const int batch_size = batch ? line + 1 : 1;
                    send( "craft", [&]( JsonOut & json ) {
                        json.member( "recipe", chosen->ident().str() );
                        json.member( "batch", batch_size );
                    } );
                    auto &recent = uistate.recent_recipes;
                    recent.erase( std::remove( recent.begin(), recent.end(), chosen->ident() ), recent.end() );
                    recent.push_back( chosen->ident() );
                    if( recent.size() > 20 ) {
                        recent.erase( recent.begin() );
                    }
                    done = true;
                }
            }
        } else if( action == "HELP_RECIPE" && !current.empty() ) {
            const description *info = current_info();
            if( info != nullptr ) {
                ui.invalidate_ui();
                int scroll = 0;
                item_info_data data( "", "", { iteminfo( "DESCRIPTION", info->result.empty() ?
                                               current[line]->result_name() : info->result ) }, {}, scroll );
                data.handle_scrolling = true;
                data.arrow_scrolling = true;
                const int info_width = std::min( TERMX, FULL_SCREEN_WIDTH );
                const int info_height = std::min( TERMY, FULL_SCREEN_HEIGHT );
                iteminfo_window info_window( data, point( ( TERMX - info_width ) / 2,
                                             ( TERMY - info_height ) / 2 ), info_width, info_height );
                info_window.execute();
            }
        } else if( action == "FILTER" ) {
            string_input_popup popup;
            popup
            .title( _( "Search:" ) )
            .width( 85 )
            .description( craft_screen::filter_help() )
            .desc_color( c_light_gray )
            .identifier( "craft_recipe_filter" )
            .hist_use_uilist( false )
            .edit( filterstring );
            if( popup.confirmed() ) {
                recalc = true;
                if( batch ) {
                    batch = false;
                    line = batch_line;
                }
            }
        } else if( action == "QUIT" ) {
            done = true;
        } else if( action == "RESET_FILTER" ) {
            filterstring.clear();
            recalc = true;
        } else if( action == "CYCLE_BATCH" && !current.empty() ) {
            batch = !batch;
            if( batch ) {
                batch_line = line;
                chosen = current[batch_line];
            } else {
                keepline = true;
            }
            recalc = true;
        } else if( action == "TOGGLE_FAVORITE" && !current.empty() && !batch ) {
            keepline = true;
            recalc = filterstring.empty() && subtab.cur() == "CSC_*_FAVORITE";
            if( uistate.favorite_recipes.count( current[line]->ident() ) ) {
                uistate.favorite_recipes.erase( current[line]->ident() );
                if( recalc ) {
                    if( static_cast<size_t>( line ) + 1 < current.size() ) {
                        line++;
                    } else {
                        line--;
                    }
                }
            } else {
                uistate.favorite_recipes.insert( current[line]->ident() );
            }
        } else if( action == "HIDE_SHOW_RECIPE" && !current.empty() && !batch ) {
            if( show_hidden ) {
                uistate.hidden_recipes.erase( current[line]->ident() );
            } else {
                uistate.hidden_recipes.insert( current[line]->ident() );
            }
            recalc = true;
            keepline = true;
            if( static_cast<size_t>( line ) + 1 < current.size() ) {
                line++;
            } else {
                line--;
            }
        } else if( action == "HELP_KEYBINDINGS" ) {
            ui.mark_resize();
        }
        if( line < 0 ) {
            line = current.size() - 1;
        } else if( line >= static_cast<int>( current.size() ) ) {
            line = 0;
        }
    } while( !done );
}

} // namespace mp::remote_crafting
