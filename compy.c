#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-compat.h>
#include <xkbcommon/xkbcommon.h>

#define COLOR(hex)                                                                                 \
    {((hex >> 24) & 0xFF) / 255.0f, ((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,   \
     (hex & 0xFF) / 255.0f}

enum tinywl_cursor_mode {
    TINYWL_CURSOR_PASSTHROUGH,
    TINYWL_CURSOR_MOVE,
    TINYWL_CURSOR_RESIZE,
};

struct tinywl_output {
    struct wl_list link;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct tinywl_toplevel {
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener request_decoration_mode;
    struct wl_listener destroy_decoration;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_rect *border[4];
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct tinywl_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct tinywl_keyboard {
    struct wl_list link;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

static struct wl_display *display;
static struct wlr_backend *backend;
static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
static struct wlr_scene *scene;
static struct wlr_scene_output_layout *scene_layout;

static struct wlr_xdg_shell *xdg_shell;
static struct wl_listener new_xdg_toplevel;
static struct wl_listener new_xdg_popup;
static struct wl_list toplevels;

static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_listener new_xdg_decoration;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;
static struct wl_listener cursor_motion;
static struct wl_listener cursor_motion_absolute;
static struct wl_listener cursor_button;
static struct wl_listener cursor_axis;
static struct wl_listener cursor_frame;

static struct wlr_seat *seat;
static struct wl_listener new_input;
static struct wl_listener request_cursor;
static struct wl_listener pointer_focus_change;
static struct wl_listener request_set_selection;
static struct wl_list keyboards;
static enum tinywl_cursor_mode cursor_mode;
static struct tinywl_toplevel *grabbed_toplevel;
static double grab_x, grab_y;
static struct wlr_box grab_geobox;
static uint32_t resize_edges;

static struct wlr_output_layout *output_layout;
static struct wl_list outputs;
static struct wl_listener new_output;

/* config options */
static uint32_t border_width = 1;
static float border_coor[4] = COLOR(0xff00ffff);

static void die(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
        fputc(' ', stderr);
        perror(NULL);
    } else {
        fputc('\n', stderr);
    }

    exit(EXIT_FAILURE);
}

static void focus_toplevel(struct tinywl_toplevel *toplevel) {
    /* Note: this function only deals with keyboard focus. */
    if (toplevel == NULL) {
        return;
    }
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (prev_surface == surface) {
        /* Don't re-focus an already focused surface. */
        return;
    }
    if (prev_surface) {
        /* Deactivate the previously focused surface. This lets the client know
         * it no longer has focus and the client will repaint accordingly, e.g.
         * stop displaying a caret. */
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    /* Move the toplevel to the front */
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&toplevels, &toplevel->link);
    /* Activate the new surface */
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    /* Tell the seat to have the keyboard enter this surface. wlroots will keep
     * track of this and automatically send key events to the appropriate
     * clients without additional work on your part. */
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    /* This event is raised when a modifier key, such as shift or alt, is
     * pressed. We simply communicate this to the client. */
    struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    /* A seat can only have one keyboard, but this is a limitation of the
     * Wayland protocol - not wlroots. We assign all connected keyboards to the
     * same seat. You can swap out the underlying wlr_keyboard like this and
     * wlr_seat handles this transparently. */
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    /* Send modifiers to the client. */
    wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr_keyboard->modifiers);
}

static void spawn(char *argv[]) {
    if (fork() == 0) {
        setsid();
        execvp(argv[0], argv);
        perror("execvp");
        _exit(1);
    }
}

static bool handle_keybinding(xkb_keysym_t sym) {
    /* This function assumes LOGO is held down.
     * Here we handle compositor keybindings. This is when the compositor is
     * processing keys, rather than passing them on to the client for its own processing. */
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(display);
        break;
    case XKB_KEY_Return:
        spawn((char *[]){"/usr/bin/foot", NULL});
        break;
    default:
        return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    /* This event is raised when a key is pressed or released. */
    struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;

    assert(keyboard);

    /* Translate libinput keycode -> xkbcommon */
    uint32_t keycode = event->keycode + 8;
    /* Get a list of keysyms based on the keymap for this keyboard */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    // xkb_keysym_t sym = xkb_keysym_from_name("F", XKB_KEYSYM_CASE_INSENSITIVE);
    // printf("%d, %d\n", sym, syms[0]);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if ((modifiers & WLR_MODIFIER_LOGO) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(syms[i]);
        }
    }

    if (!handled) {
        /* Otherwise, we pass it along to the client. */
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void new_keyboard(struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->wlr_keyboard = wlr_keyboard;

    /* We need to prepare an XKB keymap and assign it to the keyboard. This
     * assumes the defaults (e.g. layout = "us"). */
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    /* Here we set up listeners for keyboard events. */
    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);

    /* And add the keyboard to our list of keyboards */
    wl_list_insert(&keyboards, &keyboard->link);
}

static void new_pointer(struct wlr_input_device *device) {
    /* We don't do anything special with pointers. All of our pointer handling
     * is proxied through wlr_cursor. On another compositor, you might take this
     * opportunity to do libinput configuration on the device to set acceleration, etc. */
    wlr_cursor_attach_input_device(cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    /* This event is raised by the backend when a new input device become available. */
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        new_pointer(device);
        break;
    default:
        break;
    }
    /* We need to let the wlr_seat know what our capabilities are, which is
     * communiciated to the client. In TinyWL we always have a cursor, even if
     * there are no pointer devices, so we always include that capability. */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    /* This event is raised by the seat when a client provides a cursor image */
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = seat->pointer_state.focused_client;
    /* This can be sent by any client, so we check to make sure this one is
     * actually has pointer focus first. */
    if (focused_client == event->seat_client) {
        /* Once we've vetted the client, we can tell the cursor to use the
         * provided surface as the cursor image. It will set the hardware cursor
         * on the output that it's currently on and continue to do so as the
         * cursor moves between outputs. */
        wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
    /* This event is raised when the pointer focus is changed, including when
     * the client is closed. We set the cursor image to its default if target
     * surface is NULL. */
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (event->new_surface == NULL) {
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    /* This event is raised by the seat when a client wants to set the
     * selection, usually when the user copies something. wlroots allows
     * compositors to ignore such requests if they so choose, but in tinywl we
     * always honor */
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(seat, event->source, event->serial);
}

static struct tinywl_toplevel *
desktop_toplevel_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
    /* This returns the topmost node in the scene at the given layout coords.
     * We only care about surface nodes as we are specifically looking for a
     * surface in the surface tree of a tinywl_toplevel. */
    struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }

    *surface = scene_surface->surface;
    /* Find the node corresponding to the tinywl_toplevel at the root of this
     * surface tree, it is the only one for which we set the data field. */
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree->node.data;
}

static void begin_interactive(struct tinywl_toplevel *toplevel, enum tinywl_cursor_mode mode,
                              uint32_t edges) {
    /* This function sets up an interactive move or resize operation, where the
     * compositor stops propagating pointer events to clients and instead
     * consumes them itself, to move or resize windows. */

    grabbed_toplevel = toplevel;
    cursor_mode = mode;

    if (mode == TINYWL_CURSOR_MOVE) {
        grab_x = cursor->x - toplevel->scene_tree->node.x;
        grab_y = cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
                          ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        grab_x = cursor->x - border_x;
        grab_y = cursor->y - border_y;

        grab_geobox = *geo_box;
        grab_geobox.x += toplevel->scene_tree->node.x;
        grab_geobox.y += toplevel->scene_tree->node.y;

        resize_edges = edges;
    }
}

static void reset_cursor_mode() {
    /* Reset the cursor mode to passthrough */
    cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
    grabbed_toplevel = NULL;
}

static void process_cursor_move() {
    /* Move the grabbed toplevel to the new position. */
    struct tinywl_toplevel *toplevel = grabbed_toplevel;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, cursor->x - grab_x,
                                cursor->y - grab_y);
}

static void process_cursor_resize() {
    /* Resizing the grabbed toplevel can be a little bit complicated, because
     * we could be resizing from any corner or edge. This not only resizes the
     * toplevel on one or two axes, but can also move the toplevel if you
     * resize from the top or left edges (or top-left corner).
     *
     * Note that some shortcuts are taken here. In a more fleshed-out
     * compositor, you'd wait for the client to prepare a buffer at the new
     * size, then commit any movement that was prepared. */
    struct tinywl_toplevel *toplevel = grabbed_toplevel;
    double border_x = cursor->x - grab_x;
    double border_y = cursor->y - grab_y;
    int new_left = grab_geobox.x;
    int new_right = grab_geobox.x + grab_geobox.width;
    int new_top = grab_geobox.y;
    int new_bottom = grab_geobox.y + grab_geobox.height;

    if (resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box->x,
                                new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(uint32_t time) {
    /* If the mode is non-passthrough, delegate to those functions. */
    if (cursor_mode == TINYWL_CURSOR_MOVE) {
        process_cursor_move();
        return;
    } else if (cursor_mode == TINYWL_CURSOR_RESIZE) {
        process_cursor_resize();
        return;
    }

    /* Otherwise, find the toplevel under the pointer and send the event along. */
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct tinywl_toplevel *toplevel =
        desktop_toplevel_at(cursor->x, cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        /* If there's no toplevel under the cursor, set the cursor image to a
         * default. This is what makes the cursor image appear when you move it
         * around the screen, not over any toplevels. */
        wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
    }
    if (surface) {
        /* Send pointer enter and motion events.
         *
         * The enter event gives the surface "pointer focus", which is distinct
         * from keyboard focus. You get pointer focus by moving the pointer over
         * a window.
         *
         * Note that wlroots will avoid sending duplicate enter/motion events if
         * the surface has already has pointer focus or if the client is already
         * aware of the coordinates passed. */
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        /* Clear pointer focus so future button events and such are not sent to
         * the last client to have the cursor over it. */
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    /* This event is forwarded by the cursor when a pointer emits a _relative_
     * pointer motion event (i.e. a delta) */
    struct wlr_pointer_motion_event *event = data;
    /* The cursor doesn't move unless we tell it to. The cursor automatically
     * handles constraining the motion to the output layout, as well as any
     * special configuration applied for the specific input device which
     * generated the event. You can pass NULL for the device if you want to move
     * the cursor around without any input. */
    wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    /* This event is forwarded by the cursor when a pointer emits an _absolute_
     * motion event, from 0..1 on each axis. This happens, for example, when
     * wlroots is running under a Wayland window rather than KMS+DRM, and you
     * move the mouse over the window. You could enter the window from any edge,
     * so we have to warp the mouse there. There is also some hardware which
     * emits these events. */
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct wlr_pointer_button_event *event = data;

    /* Notify the client with pointer focus that a button press has occurred */
    wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        /* If you released any buttons, we exit interactive move/resize mode. */
        reset_cursor_mode();
    } else {
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
        uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

        /* Focus that client if the button was _pressed_ */
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct tinywl_toplevel *toplevel =
            desktop_toplevel_at(cursor->x, cursor->y, &surface, &sx, &sy);
        focus_toplevel(toplevel);

        if (mods & WLR_MODIFIER_LOGO) {
            begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    /* This event is forwarded by the cursor when a pointer emits an axis event,
     * for example when you move the scroll wheel. */
    struct wlr_pointer_axis_event *event = data;
    /* Notify the client with pointer focus of the axis event. */
    wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    /* This event is forwarded by the cursor when a pointer emits an frame
     * event. Frame events are sent after regular pointer events to group
     * multiple events together. For instance, two axis events may happen at the
     * same time, in which case a frame event won't be sent in between.
     * Notify the client with pointer focus of the frame event. */
    wlr_seat_pointer_notify_frame(seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
    /* This function is called every time an output is ready to display a frame,
     * generally at the output's refresh rate (e.g. 60Hz). */
    struct tinywl_output *output = wl_container_of(listener, output, frame);

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

    /* Render the scene if needed and commit the output */
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    /* This function is called when the backend requests a new state for
     * the output. For example, Wayland and X11 backends request a new mode
     * when the output window is resized. */
    struct tinywl_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_output *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    /* This event is raised by the backend when a new output (aka a display or
     * monitor) becomes available. */
    struct wlr_output *wlr_output = data;

    /* Configures the output created by the backend to use our allocator
     * and our renderer. Must be done once, before committing the output */
    wlr_output_init_render(wlr_output, allocator, renderer);

    /* The output may be disabled, switch it on. */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    /* Some backends don't have modes. DRM+KMS does, and we need to set a mode
     * before we can use the output. The mode is a tuple of (width, height,
     * refresh rate), and each monitor supports only a specific set of modes. We
     * just pick the monitor's preferred mode, a more sophisticated compositor
     * would let the user configure it. */
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    /* Set scale of the output */
    wlr_output_state_set_scale(&state, 1);

    /* Atomically applies the new output state. */
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    /* Allocates and configures our state for this output */
    struct tinywl_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;

    /* Sets up a listener for the frame event. */
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    /* Sets up a listener for the state request event. */
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    /* Sets up a listener for the destroy event. */
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&outputs, &output->link);

    /* Adds this to the output layout. The add_auto function arranges outputs
     * from left-to-right in the order they appear. A more sophisticated
     * compositor would let the user configure the arrangement of outputs in the
     * layout.
     *
     * The output layout utility automatically adds a wl_output global to the
     * display, which Wayland clients can see to find out information about the
     * output (such as DPI, scale factor, manufacturer, etc). */
    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, wlr_output);
    wlr_scene_output_layout_add_output(scene_layout, l_output, scene_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    /* Called when the surface is mapped, or ready to display on-screen. */
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);

    /* Initialize borders. */
    for (int i = 0; i < 4; i++) {
        toplevel->border[i] = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, border_coor);
    }

    /* Expand toplevel surface geometry to accomodate borders. */
    toplevel->xdg_toplevel->base->geometry.width += 2 * border_width;
    toplevel->xdg_toplevel->base->geometry.height += 2 * border_width;

    wl_list_insert(&toplevels, &toplevel->link);
    focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    /* Called when the surface is unmapped, and should no longer be shown. */
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    /* Reset the cursor mode if the grabbed toplevel was unmapped. */
    if (toplevel == grabbed_toplevel) {
        reset_cursor_mode();
    }

    wl_list_remove(&toplevel->link);

    /* Focus the next toplevel if the currently focused one is being unmapped */
    bool is_focused = toplevel->xdg_toplevel->base->surface == seat->keyboard_state.focused_surface;
    if (!wl_list_empty(&toplevels) && is_focused) {
        struct tinywl_toplevel *next_toplevel =
            wl_container_of(toplevels.prev, next_toplevel, link);
        focus_toplevel(next_toplevel);
    }
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        /* When an xdg_surface performs an initial commit, the compositor must
         * reply with a configure so the client can map the surface. tinywl
         * configures the xdg_toplevel with 0,0 size to let the client pick the
         * dimensions itself.
         */
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 500, 500);
        return;
    }

    struct wlr_box geom = toplevel->xdg_toplevel->base->geometry;
    wlr_scene_rect_set_size(toplevel->border[0], geom.width, border_width);
    wlr_scene_rect_set_size(toplevel->border[1], geom.width, border_width);
    wlr_scene_rect_set_size(toplevel->border[2], border_width, geom.height - 2 * border_width);
    wlr_scene_rect_set_size(toplevel->border[3], border_width, geom.height - 2 * border_width);
    wlr_scene_node_set_position(&toplevel->border[1]->node, 0, geom.height - border_width);
    wlr_scene_node_set_position(&toplevel->border[2]->node, 0, border_width);
    wlr_scene_node_set_position(&toplevel->border[3]->node, geom.width - border_width,
                                border_width);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);

    free(toplevel);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    /* This event is raised when a client would like to begin an interactive
     * move, typically because the user clicked on their client-side
     * decorations. Note that a more sophisticated compositor should check the
     * provided serial against a list of button press serials sent to this
     * client, to prevent the client from requesting this whenever they want. */
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    /* This event is raised when a client would like to begin an interactive
     * resize, typically because the user clicked on their client-side
     * decorations. Note that a more sophisticated compositor should check the
     * provided serial against a list of button press serials sent to this
     * client, to prevent the client from requesting this whenever they want. */
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    /* This event is raised when a client would like to maximize itself,
     * typically because the user clicked on the maximize button on client-side
     * decorations. tinywl doesn't support maximization, but to conform to
     * xdg-shell protocol we still must send a configure.
     * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
     * However, if the request was sent before an initial commit, we don't do
     * anything and let the client finish the initial surface setup. */
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    /* Just as with request_maximize, we must send a configure here. */
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    /* This event is raised when a client creates a new toplevel (application window). */
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    /* Allocate a tinywl_toplevel for this surface */
    struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->xdg_toplevel->base->data = toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&scene->tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;

    /* Listen to the various events it can emit */
    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    /* cotd */
    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct tinywl_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        /* When an xdg_surface performs an initial commit, the compositor must
         * reply with a configure so the client can map the surface.
         * tinywl sends an empty configure. A more sophisticated compositor
         * might change an xdg_popup's geometry to ensure it's not positioned
         * off-screen, for example. */
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    /* This event is raised when a client creates a new popup. */
    struct wlr_xdg_popup *xdg_popup = data;

    struct tinywl_popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;

    /* We must add xdg popups to the scene graph so they get rendered. The
     * wlroots scene graph provides a helper for this, but to use it we must
     * provide the proper parent scene node of the xdg popup. To enable this,
     * we always set the user data field of xdg_surfaces to the corresponding
     * scene node. */
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    struct tinywl_toplevel *toplevel = parent->data;
    struct wlr_scene_tree *parent_tree = toplevel->scene_tree;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void request_decoration_mode(struct wl_listener *listener, void *data) {
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_decoration_mode);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
                                                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

static void destroy_decoration(struct wl_listener *listener, void *data) {
    struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, destroy_decoration);

    wl_list_remove(&toplevel->destroy_decoration.link);
    wl_list_remove(&toplevel->request_decoration_mode.link);
}

static void server_new_xdg_decoration(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct tinywl_toplevel *toplevel = deco->toplevel->base->data;
    toplevel->decoration = deco;

    toplevel->request_decoration_mode.notify = request_decoration_mode;
    wl_signal_add(&deco->events.request_mode, &toplevel->request_decoration_mode);
    toplevel->destroy_decoration.notify = destroy_decoration;
    wl_signal_add(&deco->events.destroy, &toplevel->destroy_decoration);

    request_decoration_mode(&toplevel->request_decoration_mode, deco);
}

static void setup() {
    /* The Wayland display is managed by libwayland. It handles accepting
     * clients from the Unix socket, managing Wayland globals, and so on. */
    display = wl_display_create();
    /* The backend is a wlroots feature which abstracts the underlying input and
     * output hardware. The autocreate option will choose the most suitable
     * backend based on the current environment, such as opening an X11 window
     * if an X11 server is running. */
    backend = wlr_backend_autocreate(wl_display_get_event_loop(display), NULL);
    if (backend == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return;
    }

    /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
     * can also specify a renderer using the WLR_RENDERER env var.
     * The renderer is responsible for defining the various pixel formats it
     * supports for shared memory, this configures that for clients. */
    renderer = wlr_renderer_autocreate(backend);
    if (renderer == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return;
    }

    wlr_renderer_init_wl_display(renderer, display);

    /* Autocreates an allocator for us.
     * The allocator is the bridge between the renderer and the backend. It
     * handles the buffer creation, allowing wlroots to render onto the
     * screen */
    allocator = wlr_allocator_autocreate(backend, renderer);
    if (allocator == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return;
    }

    /* This creates some hands-off wlroots interfaces. The compositor is
     * necessary for clients to allocate surfaces, the subcompositor allows to
     * assign the role of subsurfaces to surfaces and the data device manager
     * handles the clipboard. Each of these wlroots interfaces has room for you
     * to dig your fingers in and play with their behavior if you want. Note
     * that the clients cannot set the selection directly without compositor
     * approval, see the handling of the request_set_selection event below. */
    wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_viewporter_create(display);
    wlr_screencopy_manager_v1_create(display);
    wlr_single_pixel_buffer_manager_v1_create(display);
    wlr_presentation_create(display, backend, 2);

    /* Creates an output layout, which a wlroots utility for working with an
     * arrangement of screens in a physical layout. */
    output_layout = wlr_output_layout_create(display);

    /* Configure a listener to be notified when new outputs are available on the
     * backend. */
    wl_list_init(&outputs);
    new_output.notify = server_new_output;
    wl_signal_add(&backend->events.new_output, &new_output);

    /* Create a scene graph. This is a wlroots abstraction that handles all
     * rendering and damage tracking. All the compositor author needs to do
     * is add things that should be rendered to the scene graph at the proper
     * positions and then call wlr_scene_output_commit() to render a frame if
     * necessary. */
    scene = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

    /* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
     * used for application windows. For more detail on shells, refer to
     * https://drewdevault.com/2018/07/29/Wayland-shells.html. */
    wl_list_init(&toplevels);
    xdg_shell = wlr_xdg_shell_create(display, 3);
    new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
    new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

    xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(display);
    new_xdg_decoration.notify = server_new_xdg_decoration;
    wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

    /* Creates a cursor, which is a wlroots utility for tracking the cursor
     * image shown on screen. */
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, output_layout);

    /* Creates an xcursor manager, another wlroots utility which loads up
     * Xcursor themes to source cursor images from and makes sure that cursor
     * images are available at all scale factors on the screen (necessary for
     * HiDPI support). */
    cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    /* wlr_cursor *only* displays an image on screen. It does not move around
     * when the pointer moves. However, we can attach input devices to it, and
     * it will generate aggregate events for all of them. In these events, we
     * can choose how we want to process them, forwarding them to clients and
     * moving the cursor around. More detail on this process is described in
     * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
     *
     * And more comments are sprinkled throughout the notify functions above. */
    cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
    cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&cursor->events.motion, &cursor_motion);
    cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
    cursor_button.notify = server_cursor_button;
    wl_signal_add(&cursor->events.button, &cursor_button);
    cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&cursor->events.axis, &cursor_axis);
    cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&cursor->events.frame, &cursor_frame);

    /* Configures a seat, which is a single "seat" at which a user sits and
     * operates the computer. This conceptually includes up to one keyboard,
     * pointer, touch, and drawing tablet device. We also rig up a listener to
     * let us know when new input devices are available on the backend. */
    wl_list_init(&keyboards);
    new_input.notify = server_new_input;
    wl_signal_add(&backend->events.new_input, &new_input);
    seat = wlr_seat_create(display, "seat0");
    request_cursor.notify = seat_request_cursor;
    wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
    pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&seat->pointer_state.events.focus_change, &pointer_focus_change);
    request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&seat->events.request_set_selection, &request_set_selection);
}

static void run(char *startup_cmd) {
    /* Add a Unix socket to the Wayland display. */
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        wlr_backend_destroy(backend);
        return;
    }

    /* Start the backend. This will enumerate outputs and inputs, become the DRM
     * master, etc */
    if (!wlr_backend_start(backend)) {
        wlr_backend_destroy(backend);
        wl_display_destroy(display);
        return;
    }

    /* Set the WAYLAND_DISPLAY environment variable to our socket and run the
     * startup command if requested. */
    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
        }
    }
    /* Run the Wayland event loop. This does not return until you exit the
     * compositor. Starting the backend rigged up all of the necessary event
     * loop configuration to listen to libinput events, DRM events, generate
     * frame events at the refresh rate, and so on. */
    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(display);
}

static void cleanup() {
    wl_display_destroy_clients(display);

    wl_list_remove(&new_xdg_toplevel.link);
    wl_list_remove(&new_xdg_popup.link);

    wl_list_remove(&new_xdg_decoration.link);

    wl_list_remove(&cursor_motion.link);
    wl_list_remove(&cursor_motion_absolute.link);
    wl_list_remove(&cursor_button.link);
    wl_list_remove(&cursor_axis.link);
    wl_list_remove(&cursor_frame.link);

    wl_list_remove(&new_input.link);
    wl_list_remove(&request_cursor.link);
    wl_list_remove(&pointer_focus_change.link);
    wl_list_remove(&request_set_selection.link);

    wl_list_remove(&new_output.link);

    wlr_scene_node_destroy(&scene->tree.node);
    wlr_xcursor_manager_destroy(cursor_mgr);
    wlr_cursor_destroy(cursor);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
}

void usage(const char *prog_name) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -h        Show this help message\n"
            "  -s        Startup command\n",
            prog_name);
}

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);
    char *startup_cmd = NULL;

    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            die("Try '%s -h' for more information.", argv[0]);
        }
    }
    if (optind < argc) {
        die("Try '%s -h' for more information.", argv[0]);
    }

    setup();
    run(startup_cmd);
    cleanup();

    return EXIT_SUCCESS;
}
