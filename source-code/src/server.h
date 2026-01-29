#ifndef HACKERLAND_SERVER_H
#define HACKERLAND_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

#if HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

struct hk_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *output_layout;
    struct wlr_compositor *compositor;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;

    #if HAS_XWAYLAND
    struct wlr_xwayland *xwayland;
    #endif

    // Listeners
    struct wl_listener new_output;
    struct wl_listener new_xdg_surface;
    #if HAS_XWAYLAND
    struct wl_listener new_xwayland_surface;
    #endif
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;

    struct wl_list outputs; // hk_output::link
    struct wl_list views;   // hk_view::link
    struct wl_list keyboards;

    // Visual Configuration
    struct {
        int gap_size;
        int padding;
        int bar_height;
        float border_width;
        float active_opacity;
        float inactive_opacity;
        float animation_speed;
        float color_active_border[4];
        float color_inactive_border[4];
    } config;
};

struct hk_output {
    struct wl_list link;
    struct hk_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

enum hk_view_type {
    HK_VIEW_XDG,
    HK_VIEW_XWAYLAND
};

struct hk_geometry {
    double x, y;
    double width, height;
};

struct hk_view {
    struct wl_list link;
    struct hk_server *server;
    enum hk_view_type type;

    struct wlr_xdg_toplevel *xdg_toplevel;
    #if HAS_XWAYLAND
    struct wlr_xwayland_surface *xwayland_surface;
    #endif

    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_rect *border;

    bool mapped;

    // Animation State
    struct hk_geometry current;
    struct hk_geometry target;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener destroy;
    struct wl_listener request_configure; // Specifically for XWayland
};

struct hk_keyboard {
    struct wl_list link;
    struct hk_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

// Function Prototypes
void server_new_output(struct wl_listener *listener, void *data);
void server_new_xdg_surface(struct wl_listener *listener, void *data);
#if HAS_XWAYLAND
void server_new_xwayland_surface(struct wl_listener *listener, void *data);
#endif
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_new_keyboard(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);

void arrange_windows(struct hk_server *server);
void update_view_animations(struct hk_server *server, long delta_time);
void init_ui(struct hk_server *server);

#endif
