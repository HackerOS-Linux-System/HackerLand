#include "server.h"
#include <stdlib.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <sys/wait.h>

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct hk_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                       &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct hk_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

        // Safety: Alt + Escape = Quit (Prevents locking up in TTY)
        if ((modifiers & WLR_MODIFIER_ALT) && event->keycode == KEY_ESC) {
            wl_display_terminate(keyboard->server->wl_display);
            return;
        }

        // Feature: Alt + Enter = Spawn Terminal
        if ((modifiers & WLR_MODIFIER_ALT) && event->keycode == KEY_ENTER) {
            if (fork() == 0) {
                // Try common terminals
                execl("/bin/sh", "/bin/sh", "-c", "weston-terminal || kitty || alacritty || gnome-terminal", NULL);
                exit(0);
            }
            // Consumed
            return;
        }
    }

    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(keyboard->server->seat, event->time_msec,
                                 event->keycode, event->state);
}

// Helper for keyboard creation
static void create_keyboard(struct hk_server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct hk_keyboard *keyboard = calloc(1, sizeof(struct hk_keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

void server_new_input(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        create_keyboard(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        // Attach pointer to cursor so it can move
        wlr_cursor_attach_input_device(server->cursor, device);
        // FIX: Set visible cursor immediately when pointer is attached
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
    }

    // Update capabilities (e.g. if a pointer is added, tell clients we have a pointer)
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);

    // Ensure cursor is visible
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);

    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}

void server_cursor_button(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat,
                                 event->time_msec,
                                 event->orientation,
                                 event->delta,
                                 event->delta_discrete,
                                 event->source,
                                 event->relative_direction
    );
}
