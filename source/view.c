/**
 * rofi
 *
 * MIT/X11 License
 * Modified 2016-2017 Qball Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <locale.h>

#include <cairo.h>

#include "timings.h"
#include "settings.h"

#include "rofi.h"
#include "mode.h"
#include "xkb-internal.h"
#include "helper.h"
#include "helper-theme.h"
#include "wayland.h"
#include "x11-helper.h"
#include "xrmoptions.h"
#include "dialogs/dialogs.h"

#include "view.h"
#include "view-internal.h"

#include "theme.h"

/** The Rofi View log domain */
#define LOG_DOMAIN    "View"

/**
 * @param state The handle to the view
 * @param qr    Indicate if queue_redraw should be called on changes.
 *
 * Update the state of the view. This involves filter state.
 */
void rofi_view_update ( RofiViewState *state, gboolean qr );

static int rofi_view_calculate_height ( RofiViewState *state );

/** Thread pool used for filtering */
GThreadPool *tpool = NULL;

/** Global pointer to the currently active RofiViewState */
RofiViewState *current_active_menu = NULL;

/**
 * Structure holding cached state.
 */
struct
{
    /** Cairo Surface for edit_pixmap */
    cairo_surface_t    *edit_surf;
    /** Drawable context for edit_surf */
    cairo_t            *edit_draw;
    /** Main flags */
    MenuFlags          flags;
    /** List of stacked views */
    GQueue             views;
    /** timeout for reloading */
    guint              idle_timeout;
    /** debug counter for redraws */
    unsigned long long count;
    /** redraw idle time. */
    guint              repaint_source;
    /** Window fullscreen */
    gboolean           fullscreen;
} CacheState = {
    .edit_surf      = NULL,
    .edit_draw      = NULL,
    .flags          = MENU_NORMAL,
    .views          = G_QUEUE_INIT,
    .idle_timeout   =               0,
    .count          =              0L,
    .repaint_source =               0,
    .fullscreen     = FALSE,
};

static char * get_matching_state ( void )
{
    if ( config.case_sensitive ) {
        if ( config.sort ) {
            return "±";
        }
        else {
            return "-";
        }
    }
    else{
        if ( config.sort ) {
            return "+";
        }
    }
    return " ";
}

/**
 * Levenshtein Sorting.
 */
static int lev_sort ( const void *p1, const void *p2, void *arg )
{
    const int *a         = p1;
    const int *b         = p2;
    int       *distances = arg;

    return distances[*a] - distances[*b];
}

/**
 * Stores a screenshot of Rofi at that point in time.
 */
static void menu_capture_screenshot ( void )
{
    const char *outp = g_getenv ( "ROFI_PNG_OUTPUT" );
    if ( CacheState.edit_surf == NULL ) {
        // Nothing to store.
        fprintf ( stderr, "There is no rofi surface to store\n" );
        return;
    }
    const char *xdg_pict_dir = g_get_user_special_dir ( G_USER_DIRECTORY_PICTURES );
    if ( outp == NULL && xdg_pict_dir == NULL ) {
        fprintf ( stderr, "XDG user picture directory or ROFI_PNG_OUTPUT is not set. Cannot store screenshot.\n" );
        return;
    }
    // Get current time.
    GDateTime *now = g_date_time_new_now_local ();
    // Format filename.
    char      *timestmp = g_date_time_format ( now, "rofi-%Y-%m-%d-%H%M" );
    char      *filename = g_strdup_printf ( "%s.png", timestmp );
    // Build full path
    char      *fpath = NULL;
    if ( outp == NULL ) {
        int index = 0;
        fpath = g_build_filename ( xdg_pict_dir, filename, NULL );
        while ( g_file_test ( fpath, G_FILE_TEST_EXISTS ) && index < 99 ) {
            g_free ( fpath );
            g_free ( filename );
            // Try the next index.
            index++;
            // Format filename.
            filename = g_strdup_printf ( "%s-%d.png", timestmp, index );
            // Build full path
            fpath = g_build_filename ( xdg_pict_dir, filename, NULL );
        }
    }
    else {
        fpath = g_strdup ( outp );
    }
    fprintf ( stderr, color_green "Storing screenshot %s\n"color_reset, fpath );
    cairo_status_t status = cairo_surface_write_to_png ( CacheState.edit_surf, fpath );
    if ( status != CAIRO_STATUS_SUCCESS ) {
        fprintf ( stderr, "Failed to produce screenshot '%s', got error: '%s'\n", fpath,
                  cairo_status_to_string ( status ) );
    }
    g_free ( fpath );
    g_free ( filename );
    g_free ( timestmp );
    g_date_time_unref ( now );
}

static gboolean rofi_view_repaint ( G_GNUC_UNUSED void * data  )
{
    if ( current_active_menu  ) {
        // Repaint the view (if needed).
        // After a resize the edit_pixmap surface might not contain anything anymore.
        // If we already re-painted, this does nothing.
        rofi_view_update ( current_active_menu, FALSE );
        g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "expose event" );
        TICK_N ( "Expose" );
        // FIXME: draw
        TICK_N ( "flush" );
        CacheState.repaint_source = 0;
    }
    return G_SOURCE_REMOVE;
}

static void rofi_view_update_prompt ( RofiViewState *state )
{
    if ( state->prompt ) {
        const char *str = mode_get_display_name ( state->sw );
        if ( ( state->menu_flags & MENU_PROMPT_COLON ) != 0 ) {
            char *pr = g_strconcat ( str, ":", NULL );
            textbox_text ( state->prompt, pr );
            g_free ( pr );
        }
        else {
            textbox_text ( state->prompt, str );
        }
    }
}

static void rofi_view_window_update_size ( RofiViewState * state )
{
    // FIXME: Display it.
    cairo_destroy ( CacheState.edit_draw );
    cairo_surface_destroy ( CacheState.edit_surf );

    // FIXME: get next buffer
    wayland_buffer_pool_free(state->pool);
    state->pool = wayland_buffer_pool_new(state->width, state->height);

    g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Re-size window based internal request: %dx%d.", state->width, state->height );
    // Should wrap main window in a widget.
    widget_resize ( WIDGET ( state->main_window ), state->width, state->height );
}

static void rofi_view_reload_message_bar ( RofiViewState *state )
{
    if ( state->mesg_box == NULL ) {
        return;
    }
    char *msg = mode_get_message ( state->sw );
    if ( msg ) {
        textbox_text ( state->mesg_tb, msg );
        widget_enable ( WIDGET ( state->mesg_box ) );
        g_free ( msg );
    }
    else {
        widget_disable ( WIDGET ( state->mesg_box ) );
    }
}

static gboolean rofi_view_reload_idle ( G_GNUC_UNUSED gpointer data )
{
    if ( current_active_menu ) {
        current_active_menu->reload   = TRUE;
        current_active_menu->refilter = TRUE;
        rofi_view_queue_redraw ();
    }
    CacheState.idle_timeout = 0;
    return G_SOURCE_REMOVE;
}

void rofi_view_reload ( void  )
{
    // @TODO add check if current view is equal to the callee
    if ( CacheState.idle_timeout == 0 ) {
        CacheState.idle_timeout = g_timeout_add ( 1000 / 10, rofi_view_reload_idle, NULL );
    }
}

static gboolean view_changed = FALSE;
static gboolean frame_callback = FALSE;
static void rofi_view_maybe_redraw ( void  )
{
    if ( ! frame_callback )
        return;

    // FIXME: has the view changed?
    // FIXME: maybe we have a var already?
    if ( ! view_changed )
        return;

    // FIXME: redraw

    frame_callback = FALSE;
}

void rofi_view_frame_callback(void)
{
    frame_callback = TRUE;
    rofi_view_maybe_redraw();
}

void rofi_view_queue_redraw ( void  )
{
    // FIXME: maybe we have a var already?
    view_changed = TRUE;
    rofi_view_maybe_redraw();
}

void rofi_view_restart ( RofiViewState *state )
{
    state->quit = FALSE;
    state->retv = MENU_CANCEL;
}

RofiViewState * rofi_view_get_active ( void )
{
    return current_active_menu;
}

void rofi_view_set_active ( RofiViewState *state )
{
    if ( current_active_menu != NULL && state != NULL ) {
        g_queue_push_head ( &( CacheState.views ), current_active_menu );
        // TODO check.
        current_active_menu = state;
        g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "stack view." );
        rofi_view_window_update_size ( current_active_menu );
        rofi_view_queue_redraw ();
        return;
    }
    else if ( state == NULL && !g_queue_is_empty ( &( CacheState.views ) ) ) {
        g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "pop view." );
        current_active_menu = g_queue_pop_head ( &( CacheState.views ) );
        rofi_view_window_update_size ( current_active_menu );
        rofi_view_queue_redraw ();
        return;
    }
    g_assert ( ( current_active_menu == NULL && state != NULL ) || ( current_active_menu != NULL && state == NULL ) );
    current_active_menu = state;
    rofi_view_queue_redraw ();
}

void rofi_view_set_selected_line ( RofiViewState *state, unsigned int selected_line )
{
    state->selected_line = selected_line;
    // Find the line.
    unsigned int selected = 0;
    for ( unsigned int i = 0; ( ( state->selected_line ) ) < UINT32_MAX && !selected && i < state->filtered_lines; i++ ) {
        if ( state->line_map[i] == ( state->selected_line ) ) {
            selected = i;
            break;
        }
    }
    listview_set_selected ( state->list_view, selected );
    // FIXME: draw new state
}

void rofi_view_free ( RofiViewState *state )
{
    if ( state->tokens ) {
        tokenize_free ( state->tokens );
        state->tokens = NULL;
    }
    // Do this here?
    // Wait for final release?
    widget_free ( WIDGET ( state->main_window ) );
    widget_free ( WIDGET ( state->overlay ) );

    g_free ( state->line_map );
    g_free ( state->distance );
    // Free the switcher boxes.
    // When state is free'ed we should no longer need these.
    if ( config.sidebar_mode == TRUE ) {
        g_free ( state->modi );
        state->num_modi = 0;
    }
    g_free ( state );
}

MenuReturn rofi_view_get_return_value ( const RofiViewState *state )
{
    return state->retv;
}

unsigned int rofi_view_get_selected_line ( const RofiViewState *state )
{
    return state->selected_line;
}

unsigned int rofi_view_get_next_position ( const RofiViewState *state )
{
    unsigned int next_pos = state->selected_line;
    unsigned int selected = listview_get_selected ( state->list_view );
    if ( ( selected + 1 ) < state->num_lines ) {
        ( next_pos ) = state->line_map[selected + 1];
    }
    return next_pos;
}

unsigned int rofi_view_get_completed ( const RofiViewState *state )
{
    return state->quit;
}

const char * rofi_view_get_user_input ( const RofiViewState *state )
{
    if ( state->text ) {
        return state->text->text;
    }
    return NULL;
}

/**
 * Create a new, 0 initialized RofiViewState structure.
 *
 * @returns a new 0 initialized RofiViewState
 */
static RofiViewState * __rofi_view_state_create ( void )
{
    return g_malloc0 ( sizeof ( RofiViewState ) );
}
/**
 * Structure with data to process by each worker thread.
 */
typedef struct _thread_state
{
    RofiViewState *state;
    unsigned int  start;
    unsigned int  stop;
    unsigned int  count;
    GCond         *cond;
    GMutex        *mutex;
    unsigned int  *acount;

    const char    *pattern;
    glong         plen;
    void ( *callback )( struct _thread_state *t, gpointer data );
}thread_state;
/**
 * @param data A thread_state object.
 * @param user_data User data to pass to thread_state callback
 *
 * Small wrapper function that is internally used to pass a job to a worker.
 */
static void rofi_view_call_thread ( gpointer data, gpointer user_data )
{
    thread_state *t = (thread_state *) data;
    t->callback ( t, user_data );
    g_mutex_lock ( t->mutex );
    ( *( t->acount ) )--;
    g_cond_signal ( t->cond );
    g_mutex_unlock ( t->mutex );
}

static void filter_elements ( thread_state *t, G_GNUC_UNUSED gpointer user_data )
{
    for ( unsigned int i = t->start; i < t->stop; i++ ) {
        int match = mode_token_match ( t->state->sw, t->state->tokens, i );
        // If each token was matched, add it to list.
        if ( match ) {
            t->state->line_map[t->start + t->count] = i;
            if ( config.sort ) {
                // This is inefficient, need to fix it.
                char  * str = mode_get_completion ( t->state->sw, i );
                glong slen  = g_utf8_strlen ( str, -1 );
                if ( config.levenshtein_sort || config.matching_method != MM_FUZZY  ) {
                    t->state->distance[i] = levenshtein ( t->pattern, t->plen, str, slen );
                }
                else {
                    t->state->distance[i] = rofi_scorer_fuzzy_evaluate ( t->pattern, t->plen, str, slen );
                }
                g_free ( str );
            }
            t->count++;
        }
    }
}
void __create_window ( MenuFlags menu_flags )
{
    // FIXME: create surface
    // FIXME: roll next buffer

    TICK_N ( "create cairo surface" );
    // Set up pango context.
    cairo_font_options_t *fo = cairo_font_options_create ();
    // Take font description from xlib surface
    cairo_surface_get_font_options ( CacheState.edit_surf, fo );
    // TODO should we update the drawable each time?
    PangoContext *p = pango_cairo_create_context ( CacheState.edit_draw );
    // Set the font options from the xlib surface
    pango_cairo_context_set_font_options ( p, fo );
    TICK_N ( "pango cairo font setup" );

    CacheState.flags       = menu_flags;
    // Setup dpi
    // FIXME: use scale, cairo_surface_set_device_scale
    // Setup font.
    // Dummy widget.
    container *win  = container_create ( "window.box" );
    char      *font = rofi_theme_get_string ( WIDGET ( win ), "font", config.menu_font );
    if ( font ) {
        PangoFontDescription *pfd = pango_font_description_from_string ( font );
        if ( helper_validate_font ( pfd, font ) ) {
            pango_context_set_font_description ( p, pfd );
        }
        pango_font_description_free ( pfd );
    }
    PangoLanguage *l = pango_language_get_default ();
    pango_context_set_language ( p, l );
    TICK_N ( "configure font" );

    // Tell textbox to use this context.
    textbox_set_pango_context ( font, p );
    // cleanup
    g_object_unref ( p );
    cairo_font_options_destroy ( fo );

    widget_free ( WIDGET ( win ) );
    TICK_N ( "done" );
}

/**
 * @param state Internal state of the menu.
 *
 * Calculate the width of the window and the width of an element.
 */
static void rofi_view_calculate_window_width ( RofiViewState *state )
{
    if ( config.menu_width < 0 ) {
        double fw = textbox_get_estimated_char_width ( );
        state->width  = -( fw * config.menu_width );
        state->width += widget_padding_get_padding_width ( WIDGET ( state->main_window ) );
    }
    else{
        // Calculate as float to stop silly, big rounding down errors.
        state->width = config.menu_width < 101 ? ( /* FIXME: ??? what to use instead of monitor size here? */ 1000. / 100.0f ) * ( float ) config.menu_width : config.menu_width;
    }
    // Use theme configured width, if set.
    Distance width = rofi_theme_get_distance ( WIDGET ( state->main_window ), "width", state->width );
    state->width = distance_get_pixel ( width, ORIENTATION_HORIZONTAL );
}

/**
 * Nav helper functions, to avoid duplicate code.
 */

/**
 * @param state The current RofiViewState
 *
 * Tab handling.
 */
static void rofi_view_nav_row_tab ( RofiViewState *state )
{
    if ( state->filtered_lines == 1 ) {
        state->retv              = MENU_OK;
        ( state->selected_line ) = state->line_map[listview_get_selected ( state->list_view )];
        state->quit              = 1;
        return;
    }

    // Double tab!
    if ( state->filtered_lines == 0 && ROW_TAB == state->prev_action ) {
        state->retv              = MENU_NEXT;
        ( state->selected_line ) = 0;
        state->quit              = TRUE;
    }
    else {
        listview_nav_down ( state->list_view );
    }
    state->prev_action = ROW_TAB;
}
/**
 * @param state The current RofiViewState
 *
 * complete current row.
 */
inline static void rofi_view_nav_row_select ( RofiViewState *state )
{
    if ( state->list_view == NULL ) {
        return;
    }
    unsigned int selected = listview_get_selected ( state->list_view );
    // If a valid item is selected, return that..
    if ( selected < state->filtered_lines ) {
        char *str = mode_get_completion ( state->sw, state->line_map[selected] );
        textbox_text ( state->text, str );
        g_free ( str );
        textbox_keybinding ( state->text, MOVE_END );
        state->refilter = TRUE;
    }
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection to first row.
 */
inline static void rofi_view_nav_first ( RofiViewState * state )
{
//    state->selected = 0;
    listview_set_selected ( state->list_view, 0 );
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection to last row.
 */
inline static void rofi_view_nav_last ( RofiViewState * state )
{
    // If no lines, do nothing.
    if ( state->filtered_lines == 0 ) {
        return;
    }
    //state->selected = state->filtered_lines - 1;
    listview_set_selected ( state->list_view, -1 );
}

static void update_callback ( textbox *t, unsigned int index, void *udata, TextBoxFontType type, gboolean full )
{
    RofiViewState *state = (RofiViewState *) udata;
    if ( full ) {
        GList *add_list = NULL;
        int   fstate    = 0;
        char  *text     = mode_get_display_value ( state->sw, state->line_map[index], &fstate, &add_list, TRUE );
        type |= fstate;
        textbox_font ( t, type );
        // Move into list view.
        textbox_text ( t, text );
        PangoAttrList *list = textbox_get_pango_attributes ( t );
        if ( list != NULL ) {
            pango_attr_list_ref ( list );
        }
        else{
            list = pango_attr_list_new ();
        }

        if ( state->tokens && config.show_match ) {
            ThemeHighlight th = { HL_BOLD | HL_UNDERLINE, { 0.0, 0.0, 0.0, 0.0 } };
            th = rofi_theme_get_highlight ( WIDGET ( t ), "highlight", th );
            helper_token_match_get_pango_attr ( th, state->tokens, textbox_get_visible_text ( t ), list );
        }
        for ( GList *iter = g_list_first ( add_list ); iter != NULL; iter = g_list_next ( iter ) ) {
            pango_attr_list_insert ( list, (PangoAttribute *) ( iter->data ) );
        }
        textbox_set_pango_attributes ( t, list );
        pango_attr_list_unref ( list );
        g_list_free ( add_list );
        g_free ( text );
    }
    else {
        int fstate = 0;
        mode_get_display_value ( state->sw, state->line_map[index], &fstate, NULL, FALSE );
        type |= fstate;
        textbox_font ( t, type );
    }
}

void rofi_view_update ( RofiViewState *state, gboolean qr )
{
    if ( !widget_need_redraw ( WIDGET ( state->main_window ) ) ) {
        return;
    }
    g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Redraw view" );
    TICK ();
    cairo_t *d = CacheState.edit_draw;
    cairo_set_operator ( d, CAIRO_OPERATOR_SOURCE );
    // Paint the background transparent.
    cairo_set_source_rgba ( d, 0, 0, 0, 0.0 );
    cairo_paint ( d );
    TICK_N ( "Background" );

    // Always paint as overlay over the background.
    cairo_set_operator ( d, CAIRO_OPERATOR_OVER );
    widget_draw ( WIDGET ( state->main_window ), d );

    if ( state->overlay ) {
        widget_draw ( WIDGET ( state->overlay ), d );
    }
    TICK_N ( "widgets" );
    cairo_surface_flush ( CacheState.edit_surf );
    if ( qr ) {
        rofi_view_queue_redraw ();
    }
}

static void rofi_view_mouse_navigation ( RofiViewState *state )
{
    // Scroll event
    /* FIXME: use wheel events
    if ( xbe->detail > 3 ) {
        if ( xbe->detail == 4 ) {
            listview_nav_up ( state->list_view );
        }
        else if ( xbe->detail == 5 ) {
            listview_nav_down ( state->list_view );
        }
        else if ( xbe->detail == 6 ) {
            listview_nav_left ( state->list_view );
        }
        else if ( xbe->detail == 7 ) {
            listview_nav_right ( state->list_view );
        }
        return;
    }
    else {
        xcb_button_press_event_t rel = *xbe;
        if ( widget_clicked ( WIDGET ( state->main_window ), &rel ) ) {
            return;
        }
    }
    */
}
static void _rofi_view_reload_row ( RofiViewState *state )
{
    g_free ( state->line_map );
    g_free ( state->distance );
    state->num_lines = mode_get_num_entries ( state->sw );
    state->line_map  = g_malloc0_n ( state->num_lines, sizeof ( unsigned int ) );
    state->distance  = g_malloc0_n ( state->num_lines, sizeof ( int ) );
    listview_set_max_lines ( state->list_view, state->num_lines );
    rofi_view_reload_message_bar ( state );
}

static void rofi_view_refilter ( RofiViewState *state )
{
    TICK_N ( "Filter start" );
    if ( state->reload ) {
        _rofi_view_reload_row ( state );
        state->reload = FALSE;
    }
    if ( state->tokens ) {
        tokenize_free ( state->tokens );
        state->tokens = NULL;
    }
    if ( strlen ( state->text->text ) > 0 ) {
        unsigned int j        = 0;
        gchar        *pattern = mode_preprocess_input ( state->sw, state->text->text );
        glong        plen     = pattern ? g_utf8_strlen ( pattern, -1 ) : 0;
        state->tokens = tokenize ( pattern, config.case_sensitive );
        /**
         * On long lists it can be beneficial to parallelize.
         * If number of threads is 1, no thread is spawn.
         * If number of threads > 1 and there are enough (> 1000) items, spawn jobs for the thread pool.
         * For large lists with 8 threads I see a factor three speedup of the whole function.
         */
        unsigned int nt = MAX ( 1, state->num_lines / 500 );
        thread_state states[nt];
        GCond        cond;
        GMutex       mutex;
        g_mutex_init ( &mutex );
        g_cond_init ( &cond );
        unsigned int count = nt;
        unsigned int steps = ( state->num_lines + nt ) / nt;
        for ( unsigned int i = 0; i < nt; i++ ) {
            states[i].state    = state;
            states[i].start    = i * steps;
            states[i].stop     = MIN ( state->num_lines, ( i + 1 ) * steps );
            states[i].count    = 0;
            states[i].cond     = &cond;
            states[i].mutex    = &mutex;
            states[i].acount   = &count;
            states[i].plen     = plen;
            states[i].pattern  = pattern;
            states[i].callback = filter_elements;
            if ( i > 0 ) {
                g_thread_pool_push ( tpool, &states[i], NULL );
            }
        }
        // Run one in this thread.
        rofi_view_call_thread ( &states[0], NULL );
        // No need to do this with only one thread.
        if ( nt > 1 ) {
            g_mutex_lock ( &mutex );
            while ( count > 0 ) {
                g_cond_wait ( &cond, &mutex );
            }
            g_mutex_unlock ( &mutex );
        }
        g_cond_clear ( &cond );
        g_mutex_clear ( &mutex );
        for ( unsigned int i = 0; i < nt; i++ ) {
            if ( j != states[i].start ) {
                memmove ( &( state->line_map[j] ), &( state->line_map[states[i].start] ), sizeof ( unsigned int ) * ( states[i].count ) );
            }
            j += states[i].count;
        }
        if ( config.sort ) {
            g_qsort_with_data ( state->line_map, j, sizeof ( int ), lev_sort, state->distance );
        }

        // Cleanup + bookkeeping.
        state->filtered_lines = j;
        g_free ( pattern );
    }
    else{
        for ( unsigned int i = 0; i < state->num_lines; i++ ) {
            state->line_map[i] = i;
        }
        state->filtered_lines = state->num_lines;
    }
    listview_set_num_elements ( state->list_view, state->filtered_lines );

    if ( config.auto_select == TRUE && state->filtered_lines == 1 && state->num_lines > 1 ) {
        ( state->selected_line ) = state->line_map[listview_get_selected ( state->list_view  )];
        state->retv              = MENU_OK;
        state->quit              = TRUE;
    }
    // Size the window.
    int height = rofi_view_calculate_height ( state );
    if ( height != state->height ) {
        state->height = height;
        rofi_view_window_update_size ( state );
        g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Resize based on re-filter" );
    }
    state->refilter = FALSE;
    TICK_N ( "Filter done" );
}
/**
 * @param state The Menu Handle
 *
 * Check if a finalize function is set, and if sets executes it.
 */
void process_result ( RofiViewState *state );
void rofi_view_finalize ( RofiViewState *state )
{
    if ( state && state->finalize != NULL ) {
        state->finalize ( state );
    }
}

gboolean rofi_view_trigger_action ( RofiViewState *state, KeyBindingAction action )
{
    gboolean ret = TRUE;
    switch ( action )
    {
    // Handling of paste
    case PASTE_PRIMARY:
    case PASTE_SECONDARY:
        // FIXME: implement clipboard
        break;
    case SCREENSHOT:
        menu_capture_screenshot ( );
        break;
    case TOGGLE_SORT:
        config.sort     = !config.sort;
        state->refilter = TRUE;
        textbox_text ( state->case_indicator, get_matching_state () );
        break;
    case MODE_PREVIOUS:
        state->retv              = MENU_PREVIOUS;
        ( state->selected_line ) = 0;
        state->quit              = TRUE;
        break;
    // Menu navigation.
    case MODE_NEXT:
        state->retv              = MENU_NEXT;
        ( state->selected_line ) = 0;
        state->quit              = TRUE;
        break;
    // Toggle case sensitivity.
    case TOGGLE_CASE_SENSITIVITY:
        config.case_sensitive    = !config.case_sensitive;
        ( state->selected_line ) = 0;
        state->refilter          = TRUE;
        textbox_text ( state->case_indicator, get_matching_state () );
        break;
    // Special delete entry command.
    case DELETE_ENTRY:
    {
        unsigned int selected = listview_get_selected ( state->list_view );
        if ( selected < state->filtered_lines ) {
            ( state->selected_line ) = state->line_map[selected];
            state->retv              = MENU_ENTRY_DELETE;
            state->quit              = TRUE;
        }
        else {
            ret = FALSE;
        }
        break;
    }
    case SELECT_ELEMENT_1:
    case SELECT_ELEMENT_2:
    case SELECT_ELEMENT_3:
    case SELECT_ELEMENT_4:
    case SELECT_ELEMENT_5:
    case SELECT_ELEMENT_6:
    case SELECT_ELEMENT_7:
    case SELECT_ELEMENT_8:
    case SELECT_ELEMENT_9:
    case SELECT_ELEMENT_10:
    {
        unsigned int index = action - SELECT_ELEMENT_1;
        if ( index < state->filtered_lines ) {
            state->selected_line = state->line_map[index];
            state->retv          = MENU_OK;
            state->quit          = TRUE;
        }
        break;
    }
    case CUSTOM_1:
    case CUSTOM_2:
    case CUSTOM_3:
    case CUSTOM_4:
    case CUSTOM_5:
    case CUSTOM_6:
    case CUSTOM_7:
    case CUSTOM_8:
    case CUSTOM_9:
    case CUSTOM_10:
    case CUSTOM_11:
    case CUSTOM_12:
    case CUSTOM_13:
    case CUSTOM_14:
    case CUSTOM_15:
    case CUSTOM_16:
    case CUSTOM_17:
    case CUSTOM_18:
    case CUSTOM_19:
    {
        state->selected_line = UINT32_MAX;
        unsigned int selected = listview_get_selected ( state->list_view );
        if ( selected < state->filtered_lines ) {
            ( state->selected_line ) = state->line_map[selected];
        }
        state->retv = MENU_QUICK_SWITCH | ( ( action - CUSTOM_1 ) & MENU_LOWER_MASK );
        state->quit = TRUE;
        break;
    }
    // If you add a binding here, make sure to add it to rofi_view_keyboard_navigation too
    case CANCEL:
        state->retv = MENU_CANCEL;
        state->quit = TRUE;
        break;
    case ROW_UP:
        listview_nav_up ( state->list_view );
        break;
    case ROW_TAB:
        rofi_view_nav_row_tab ( state );
        break;
    case ROW_DOWN:
        listview_nav_down ( state->list_view );
        break;
    case ROW_LEFT:
        listview_nav_left ( state->list_view );
        break;
    case ROW_RIGHT:
        listview_nav_right ( state->list_view );
        break;
    case PAGE_PREV:
        listview_nav_page_prev ( state->list_view );
        break;
    case PAGE_NEXT:
        listview_nav_page_next ( state->list_view );
        break;
    case ROW_FIRST:
        rofi_view_nav_first ( state );
        break;
    case ROW_LAST:
        rofi_view_nav_last ( state );
        break;
    case ROW_SELECT:
        rofi_view_nav_row_select ( state );
        break;
    // If you add a binding here, make sure to add it to textbox_keybinding too
    case MOVE_CHAR_BACK:
    case MOVE_CHAR_FORWARD:
    case CLEAR_LINE:
    case MOVE_FRONT:
    case MOVE_END:
    case REMOVE_TO_EOL:
    case REMOVE_TO_SOL:
    case REMOVE_WORD_BACK:
    case REMOVE_WORD_FORWARD:
    case REMOVE_CHAR_FORWARD:
    case MOVE_WORD_BACK:
    case MOVE_WORD_FORWARD:
    case REMOVE_CHAR_BACK:
    {
        int rc = textbox_keybinding ( state->text, action );
        if ( rc == 1 ) {
            // Entry changed.
            state->refilter = TRUE;
        }
        else if ( rc == 2 ) {
            // Movement.
        }
        break;
    }
    case ACCEPT_ALT:
    {
        unsigned int selected = listview_get_selected ( state->list_view );
        state->selected_line = UINT32_MAX;
        if ( selected < state->filtered_lines ) {
            ( state->selected_line ) = state->line_map[selected];
            state->retv              = MENU_OK;
        }
        else {
            // Nothing entered and nothing selected.
            state->retv = MENU_CUSTOM_INPUT;
        }
        state->retv |= MENU_CUSTOM_ACTION;
        state->quit  = TRUE;
        break;
    }
    case ACCEPT_CUSTOM:
    {
        state->selected_line = UINT32_MAX;
        state->retv          = MENU_CUSTOM_INPUT;
        state->quit          = TRUE;
        break;
    }
    case ACCEPT_ENTRY:
    {
        // If a valid item is selected, return that..
        unsigned int selected = listview_get_selected ( state->list_view );
        state->selected_line = UINT32_MAX;
        if ( selected < state->filtered_lines ) {
            ( state->selected_line ) = state->line_map[selected];
            state->retv              = MENU_OK;
        }
        else {
            // Nothing entered and nothing selected.
            state->retv = MENU_CUSTOM_INPUT;
        }

        state->quit = TRUE;
        break;
    }
    case NUM_ABE:
        ret = FALSE;
        break;
    }

    return ret;
}

static void rofi_view_handle_keypress ( RofiViewState *state, xkb_stuff *xkb )
{
    xkb_keysym_t key;
    char         pad[32];
    int          len = 0;
    int keycode = 0; // FIXME: use Wayland event

    key = xkb_state_key_get_one_sym ( xkb->state, keycode );

    if ( xkb->compose.state != NULL ) {
        if ( ( key != XKB_KEY_NoSymbol ) && ( xkb_compose_state_feed ( xkb->compose.state, key ) == XKB_COMPOSE_FEED_ACCEPTED ) ) {
            switch ( xkb_compose_state_get_status ( xkb->compose.state ) )
            {
            case XKB_COMPOSE_CANCELLED:
            /* Eat the keysym that cancelled the compose sequence.
             * This is default behaviour with Xlib */
            case XKB_COMPOSE_COMPOSING:
                key = XKB_KEY_NoSymbol;
                break;
            case XKB_COMPOSE_COMPOSED:
                key = xkb_compose_state_get_one_sym ( xkb->compose.state );
                len = xkb_compose_state_get_utf8 ( xkb->compose.state, pad, sizeof ( pad ) );
                break;
            case XKB_COMPOSE_NOTHING:
                break;
            }
            if ( ( key == XKB_KEY_NoSymbol ) && ( len == 0 ) ) {
                return;
            }
        }
    }

    if ( len == 0 ) {
        len = xkb_state_key_get_utf8 ( xkb->state, keycode, pad, sizeof ( pad ) );
    }

    xkb_mod_mask_t effective = xkb_state_serialize_mods ( xkb->state, XKB_STATE_MODS_EFFECTIVE );
    xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods ( xkb->state, keycode );

    // FIXME: switch to directly check is_active+is_consumed
    unsigned int   modstate = x11_canonalize_mask ( effective & ( ~consumed ) );

    if ( key != XKB_KEY_NoSymbol ) {
        KeyBindingAction action;
        action = abe_find_action ( modstate, key );
        if ( rofi_view_trigger_action ( state, action ) ) {
            return;
        }
    }

    if ( ( len > 0 ) && ( textbox_append_char ( state->text, pad, len ) ) ) {
        state->refilter = TRUE;
        return;
    }
}

/* FIXME: dispatch from Wayland callbacks
void rofi_view_itterrate ( RofiViewState *state, xcb_generic_event_t *ev, xkb_stuff *xkb )
{
    switch ( ev->response_type & ~0x80 )
    {
    case XCB_CONFIGURE_NOTIFY:
    {
        xcb_configure_notify_event_t *xce = (xcb_configure_notify_event_t *) ev;
        if ( xce->window == CacheState.main_window ) {
            if ( state->x != xce->x || state->y != xce->y ) {
                state->x = xce->x;
                state->y = xce->y;
                widget_queue_redraw ( WIDGET ( state->main_window ) );
            }
            if ( state->width != xce->width || state->height != xce->height ) {
                state->width  = xce->width;
                state->height = xce->height;

                cairo_destroy ( CacheState.edit_draw );
                cairo_surface_destroy ( CacheState.edit_surf );

                xcb_free_pixmap ( xcb->connection, CacheState.edit_pixmap );
                CacheState.edit_pixmap = xcb_generate_id ( xcb->connection );
                xcb_create_pixmap ( xcb->connection, depth->depth, CacheState.edit_pixmap, CacheState.main_window,
                                    state->width, state->height );

                CacheState.edit_surf = cairo_xcb_surface_create ( xcb->connection, CacheState.edit_pixmap, visual, state->width, state->height );
                CacheState.edit_draw = cairo_create ( CacheState.edit_surf );
                g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Re-size window based external request: %d %d\n", state->width, state->height );
                widget_resize ( WIDGET ( state->main_window ), state->width, state->height );
            }
        }
        break;
    }
    case XCB_MOTION_NOTIFY:
    {
        if ( config.click_to_exit == TRUE ) {
            state->mouse_seen = TRUE;
        }
        xcb_motion_notify_event_t xme = *( (xcb_motion_notify_event_t *) ev );
        if ( widget_motion_notify ( WIDGET ( state->main_window ), &xme ) ) {
            return;
        }
        break;
    }
    case XCB_BUTTON_PRESS:
        rofi_view_mouse_navigation ( state, (xcb_button_press_event_t *) ev );
        break;
    case XCB_BUTTON_RELEASE:
        if ( config.click_to_exit == TRUE ) {
            if ( ( CacheState.flags & MENU_NORMAL_WINDOW ) == 0 ) {
                xcb_button_release_event_t *bre = (xcb_button_release_event_t *) ev;
                if ( ( state->mouse_seen == FALSE ) && ( bre->event != CacheState.main_window ) ) {
                    state->quit = TRUE;
                    state->retv = MENU_CANCEL;
                }
            }
            state->mouse_seen = FALSE;
        }
        break;
    // Paste event.
    case XCB_SELECTION_NOTIFY:
        rofi_view_paste ( state, (xcb_selection_notify_event_t *) ev );
        break;
    case XCB_KEYMAP_NOTIFY:
    {
        xcb_keymap_notify_event_t *kne     = (xcb_keymap_notify_event_t *) ev;
        guint                     modstate = x11_get_current_mask ( xkb );
        for ( gint32 by = 0; by < 31; ++by ) {
            for ( gint8 bi = 0; bi < 7; ++bi ) {
                if ( kne->keys[by] & ( 1 << bi ) ) {
                    // X11 keycodes starts at 8
                    xkb_keysym_t key = xkb_state_key_get_one_sym ( xkb->state, ( 8 * by + bi ) + 8 );
                    abe_find_action ( modstate, key );
                }
            }
        }
        break;
    }
    case XCB_KEY_PRESS:
        rofi_view_handle_keypress ( state, xkb, (xcb_key_press_event_t *) ev );
        break;
    case XCB_KEY_RELEASE:
    {
        xcb_key_release_event_t *xkre    = (xcb_key_release_event_t *) ev;
        unsigned int            modstate = x11_canonalize_mask ( xkre->state );
        if ( modstate == 0 ) {
            abe_trigger_release ( );
        }
        break;
    }
    default:
        break;
    }
    // Update if requested.
    if ( state->refilter ) {
        rofi_view_refilter ( state );
    }
    rofi_view_update ( state, TRUE );

    if ( ( ev->response_type & ~0x80 ) == XCB_EXPOSE && CacheState.repaint_source == 0 ) {
        CacheState.repaint_source = g_idle_add_full (  G_PRIORITY_HIGH_IDLE, rofi_view_repaint, NULL, NULL );
    }
}
*/
static int rofi_view_calculate_height ( RofiViewState *state )
{
    unsigned int height = 0;
    if ( listview_get_num_lines ( state->list_view ) == 0 || CacheState.fullscreen == TRUE ) {
        height = 300; // FIXME: whatever fits
        return height;
    }

    widget *main_window = WIDGET ( state->main_window );
    height = widget_get_desired_height ( main_window );
    return height;
}

static gboolean rofi_view_modi_clicked_cb ( widget *textbox, G_GNUC_UNUSED xcb_button_press_event_t *xbe, void *udata )
{
    RofiViewState *state = ( RofiViewState *) udata;
    for ( unsigned int i = 0; i < state->num_modi; i++ ) {
        if ( WIDGET ( state->modi[i] ) == textbox ) {
            state->retv        = MENU_QUICK_SWITCH | ( i & MENU_LOWER_MASK );
            state->quit        = TRUE;
            state->skip_absorb = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}
// @TODO don't like this construction.
static void rofi_view_listview_mouse_activated_cb ( listview *lv, xcb_button_press_event_t *xce, void *udata )
{
    RofiViewState *state  = (RofiViewState *) udata;
    int           control = x11_modifier_active ( xce->state, X11MOD_CONTROL );
    state->retv = MENU_OK;
    if ( control ) {
        state->retv |= MENU_CUSTOM_ACTION;
    }
    ( state->selected_line ) = state->line_map[listview_get_selected ( lv )];
    // Quit
    state->quit        = TRUE;
    state->skip_absorb = TRUE;
}

RofiViewState *rofi_view_create ( Mode *sw,
                                  const char *input,
                                  MenuFlags menu_flags,
                                  void ( *finalize )( RofiViewState * ) )
{
    TICK ();
    RofiViewState *state = __rofi_view_state_create ();
    state->menu_flags    = menu_flags;
    state->sw            = sw;
    state->selected_line = UINT32_MAX;
    state->retv          = MENU_CANCEL;
    state->distance      = NULL;
    state->quit          = FALSE;
    state->skip_absorb   = FALSE;
    //We want to filter on the first run.
    state->refilter   = TRUE;
    state->finalize   = finalize;
    state->mouse_seen = FALSE;

    // Request the lines to show.
    state->num_lines = mode_get_num_entries ( sw );


    state->main_window = container_create ( "window.box" );
    state->main_box    = box_create ( "window.mainbox.box", BOX_VERTICAL );
    container_add ( state->main_window, WIDGET ( state->main_box ) );

    state->input_bar = box_create ( "window.mainbox.inputbar.box", BOX_HORIZONTAL );

    // Only enable widget when sidebar is enabled.
    if ( config.sidebar_mode ) {
        state->sidebar_bar = box_create ( "window.mainbox.sidebar.box", BOX_HORIZONTAL );
        box_add ( state->main_box, WIDGET ( state->sidebar_bar ), FALSE, 10 );
        state->num_modi = rofi_get_num_enabled_modi ();
        state->modi     = g_malloc0 ( state->num_modi * sizeof ( textbox * ) );
        for ( unsigned int j = 0; j < state->num_modi; j++ ) {
            const Mode * mode = rofi_get_mode ( j );
            state->modi[j] = textbox_create ( "window.mainbox.sidebar.button", TB_CENTER | TB_AUTOHEIGHT, ( mode == state->sw ) ? HIGHLIGHT : NORMAL,
                                              mode_get_display_name ( mode  ) );
            box_add ( state->sidebar_bar, WIDGET ( state->modi[j] ), TRUE, j );
            widget_set_clicked_handler ( WIDGET ( state->modi[j] ), rofi_view_modi_clicked_cb, state );
        }
    }

    // FIXME: re-add input box at the bottom?
    box_add ( state->main_box, WIDGET ( state->input_bar ), FALSE, 0 );

    state->case_indicator = textbox_create ( "window.mainbox.inputbar.case-indicator", TB_AUTOWIDTH | TB_AUTOHEIGHT, NORMAL, "*" );
    // Add small separator between case indicator and text box.
    box_add ( state->input_bar, WIDGET ( state->case_indicator ), FALSE, 3 );

    // Prompt box.
    state->prompt = textbox_create ( "window.mainbox.inputbar.prompt", TB_AUTOWIDTH | TB_AUTOHEIGHT, NORMAL, "" );
    rofi_view_update_prompt ( state );
    box_add ( state->input_bar, WIDGET ( state->prompt ), FALSE, 1 );

    // Entry box
    TextboxFlags tfl = TB_EDITABLE;
    tfl        |= ( ( menu_flags & MENU_PASSWORD ) == MENU_PASSWORD ) ? TB_PASSWORD : 0;
    state->text = textbox_create ( "window.mainbox.inputbar.entry", tfl | TB_AUTOHEIGHT, NORMAL, input );

    box_add ( state->input_bar, WIDGET ( state->text ), TRUE, 2 );

    textbox_text ( state->case_indicator, get_matching_state () );
    state->mesg_box = container_create ( "window.mainbox.message.box" );
    state->mesg_tb  = textbox_create ( "window.mainbox.message.textbox", TB_AUTOHEIGHT | TB_MARKUP | TB_WRAP, NORMAL, NULL );
    container_add ( state->mesg_box, WIDGET ( state->mesg_tb ) );
    rofi_view_reload_message_bar ( state );
    // FIXME: re-add input box at the bottom?
    box_add ( state->main_box, WIDGET ( state->mesg_box ), FALSE, 2 );

    state->overlay                = textbox_create ( "window.overlay", TB_AUTOWIDTH | TB_AUTOHEIGHT, URGENT, "blaat"  );
    state->overlay->widget.parent = WIDGET ( state->main_window );
    widget_disable ( WIDGET ( state->overlay ) );

    state->list_view = listview_create ( "window.mainbox.listview", update_callback, state, config.element_height, FALSE /* FIXME: entry-at-bottom */ );
    // Set configuration
    listview_set_multi_select ( state->list_view, ( state->menu_flags & MENU_INDICATOR ) == MENU_INDICATOR );
    listview_set_scroll_type ( state->list_view, config.scroll_method );
    listview_set_mouse_activated_cb ( state->list_view, rofi_view_listview_mouse_activated_cb, state );

    int lines = rofi_theme_get_integer ( WIDGET ( state->list_view ), "lines", config.menu_lines );
    listview_set_num_lines ( state->list_view, lines );
    listview_set_max_lines ( state->list_view, state->num_lines );

    box_add ( state->main_box, WIDGET ( state->list_view ), TRUE, 3 );

    // filtered list
    state->line_map = g_malloc0_n ( state->num_lines, sizeof ( unsigned int ) );
    state->distance = (int *) g_malloc0_n ( state->num_lines, sizeof ( int ) );

    rofi_view_calculate_window_width ( state );
    // Need to resize otherwise calculated desired height is wrong.
    widget_resize ( WIDGET ( state->main_window ), state->width, 100 );

    state->quit = FALSE;
    rofi_view_refilter ( state );
    rofi_view_update ( state, TRUE );

    // FIXME: push the surface
    // maybe rofi_view_queue_redraw() ?
    widget_queue_redraw ( WIDGET ( state->main_window ) );
    return state;
}

int rofi_view_error_dialog ( const char *msg, int markup )
{
    RofiViewState *state = __rofi_view_state_create ();
    state->retv       = MENU_CANCEL;
    state->menu_flags = MENU_ERROR_DIALOG;
    state->finalize   = process_result;

    state->main_window = container_create ( "window.box" );
    state->main_box    = box_create ( "window.mainbox.message.box", BOX_VERTICAL );
    container_add ( state->main_window, WIDGET ( state->main_box ) );
    state->text = textbox_create ( "window.mainbox.message.textbox", ( TB_AUTOHEIGHT | TB_WRAP ) + ( ( markup ) ? TB_MARKUP : 0 ),
                                   NORMAL, ( msg != NULL ) ? msg : "" );
    box_add ( state->main_box, WIDGET ( state->text ), TRUE, 1 );

    rofi_view_calculate_window_width ( state );
    // Need to resize otherwise calculated desired height is wrong.
    widget_resize ( WIDGET ( state->main_window ), state->width, 100 );
    // resize window vertically to suit
    state->height = widget_get_desired_height ( WIDGET ( state->main_window ) );

    // Move the window to the correct x,y position.
    rofi_view_window_update_size ( state );

    // Display it.
    widget_queue_redraw ( WIDGET ( state->main_window ) );

    // FIXME: push the surface
    // maybe rofi_view_queue_redraw() ?

    // Set it as current window.
    rofi_view_set_active ( state );
    return TRUE;
}

void rofi_view_hide ( void )
{
    // FIXME: quit the mainloop
}

void rofi_view_cleanup ()
{
    g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Cleanup." );
    if ( CacheState.idle_timeout > 0 ) {
        g_source_remove ( CacheState.idle_timeout );
        CacheState.idle_timeout = 0;
    }
    if ( CacheState.repaint_source > 0 ) {
        g_source_remove ( CacheState.repaint_source );
        CacheState.repaint_source = 0;
    }
    if ( CacheState.edit_draw ) {
        cairo_destroy ( CacheState.edit_draw );
        CacheState.edit_draw = NULL;
    }
    if ( CacheState.edit_surf ) {
        cairo_surface_destroy ( CacheState.edit_surf );
        CacheState.edit_surf = NULL;
    }
    g_assert ( g_queue_is_empty ( &( CacheState.views ) ) );
}
void rofi_view_workers_initialize ( void )
{
    TICK_N ( "Setup Threadpool, start" );
    if ( config.threads == 0 ) {
        config.threads = 1;
        long procs = sysconf ( _SC_NPROCESSORS_CONF );
        if ( procs > 0 ) {
            config.threads = MIN ( procs, 128l );
        }
    }
    // Create thread pool
    GError *error = NULL;
    tpool = g_thread_pool_new ( rofi_view_call_thread, NULL, config.threads, FALSE, &error );
    if ( error == NULL ) {
        // Idle threads should stick around for a max of 60 seconds.
        g_thread_pool_set_max_idle_time ( 60000 );
        // We are allowed to have
        g_thread_pool_set_max_threads ( tpool, config.threads, &error );
    }
    // If error occured during setup of pool, tell user and exit.
    if ( error != NULL ) {
        fprintf ( stderr, "Failed to setup thread pool: '%s'", error->message );
        g_error_free ( error );
        exit ( EXIT_FAILURE );
    }
    TICK_N ( "Setup Threadpool, done" );
}
void rofi_view_workers_finalize ( void )
{
    if ( tpool ) {
        g_thread_pool_free ( tpool, TRUE, FALSE );
        tpool = NULL;
    }
}
Mode * rofi_view_get_mode ( RofiViewState *state )
{
    return state->sw;
}

void rofi_view_set_overlay ( RofiViewState *state, const char *text )
{
    if ( state->overlay == NULL ) {
        return;
    }
    if ( text == NULL ) {
        widget_disable ( WIDGET ( state->overlay ) );
        return;
    }
    widget_enable ( WIDGET ( state->overlay ) );
    textbox_text ( state->overlay, text );
    int x_offset = widget_get_width ( WIDGET ( state->main_window ) );
    // Within padding of window.
    x_offset -= widget_padding_get_right ( WIDGET ( state->main_window ) );
    // Within the border of widget.
    x_offset -= widget_padding_get_right ( WIDGET ( state->main_box ) );
    x_offset -= widget_padding_get_right ( WIDGET ( state->input_bar ) );
    x_offset -= widget_get_width ( WIDGET ( state->case_indicator ) );
    x_offset -= widget_get_width ( WIDGET ( state->overlay ) );
    int top_offset = widget_padding_get_top   ( WIDGET ( state->main_window ) );
    top_offset += widget_padding_get_top   ( WIDGET ( state->main_box ) );
    widget_move ( WIDGET ( state->overlay ), x_offset, top_offset );
    // We want to queue a repaint.
    rofi_view_queue_redraw ( );
}

void rofi_view_clear_input ( RofiViewState *state )
{
    if ( state->text ) {
        textbox_text ( state->text, "" );
        rofi_view_set_selected_line ( state, 0 );
    }
}

void rofi_view_switch_mode ( RofiViewState *state, Mode *mode )
{
    state->sw = mode;
    // Update prompt;
    if ( state->prompt ) {
        rofi_view_update_prompt ( state );
        if ( config.sidebar_mode ) {
            for ( unsigned int j = 0; j < state->num_modi; j++ ) {
                const Mode * mode = rofi_get_mode ( j );
                textbox_font ( state->modi[j], ( mode == state->sw ) ? HIGHLIGHT : NORMAL );
            }
        }
    }
    rofi_view_restart ( state );
    state->reload   = TRUE;
    state->refilter = TRUE;
    rofi_view_refilter ( state );
    rofi_view_update ( state, TRUE );
}
