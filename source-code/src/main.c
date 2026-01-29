#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include "server.h"

// Global pointer for signal handling
struct hk_server *server_ptr = NULL;

void handle_signal(int signum) {
    if (server_ptr && server_ptr->wl_display) {
        // Force immediate exit if stuck
        wlr_log(WLR_INFO, "Signal %d received, shutting down...", signum);
        wl_display_terminate(server_ptr->wl_display);
    } else {
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    // 1. SAFETY FIRST: Register signals immediately.
    // This allows you to Ctrl+C if seatd hangs the GPU init.
    struct hk_server server = {0};
    server_ptr = &server;

    struct sigaction sa = {.sa_handler = handle_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    wlr_log_init(WLR_DEBUG, NULL);

    // 2. ENVIRONMENT FIXES FOR TTY/NVIDIA
    // Force software cursors (prevents invisible cursor on NVIDIA/Intel)
    setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);
    // Prefer GLES2 for stability, but allow Vulkan if specified
    if (!getenv("WLR_RENDERER")) {
        setenv("WLR_RENDERER", "gles2", 1);
    }
    // Force DRM backend if we are in TTY to avoid confusion
    if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) {
        wlr_log(WLR_INFO, "Running in TTY mode, defaulting to DRM backend");
        setenv("WLR_BACKENDS", "drm,libinput", 0);
    }

    // --- Configuration (Cyberpunk Style) ---
    server.config.gap_size = 12;
    server.config.padding = 12;
    server.config.bar_height = 36;
    server.config.border_width = 3.0f;
    server.config.active_opacity = 0.98f;
    server.config.inactive_opacity = 0.85f;
    server.config.animation_speed = 0.12f;

    // Neon Cyan/Purple scheme
    server.config.color_active_border[0] = 0.0f;
    server.config.color_active_border[1] = 0.9f;
    server.config.color_active_border[2] = 1.0f; // Cyan
    server.config.color_active_border[3] = 1.0f;

    server.config.color_inactive_border[0] = 0.3f;
    server.config.color_inactive_border[1] = 0.0f;
    server.config.color_inactive_border[2] = 0.5f; // Dark Purple
    server.config.color_inactive_border[3] = 1.0f;

    // --- Initialization ---
    server.wl_display = wl_display_create();
    struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);

    // 3. BACKEND CREATION WITH DIAGNOSTICS
    server.backend = wlr_backend_autocreate(loop, NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "CRITICAL: Failed to create backend.");

        // Print HELPFUL error message for TTY users
        fprintf(stderr, "\n\033[1;31m==================================================\033[0m\n");
        fprintf(stderr, "\033[1;31m[HACKERLAND ERROR] Hardware access denied (seatd).\033[0m\n");
        fprintf(stderr, "The compositor cannot access your video card or keyboard.\n");
        fprintf(stderr, "This is usually because 'seatd' is not running.\n\n");
        fprintf(stderr, "\033[1;33mSOLUTION (Run this first):\033[0m\n");
        fprintf(stderr, "  sudo seatd -g video &\n");
        fprintf(stderr, "  ./hackerland\n");
        fprintf(stderr, "\033[1;31m==================================================\033[0m\n\n");

        wl_display_destroy(server.wl_display);
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.wl_display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        wlr_log(WLR_ERROR, "Failed to create allocator");
        return 1;
    }

    server.scene = wlr_scene_create();
    server.scene->tree.node.data = &server;

    server.output_layout = wlr_output_layout_create(server.wl_display);
    wlr_scene_attach_output_layout(server.scene, server.output_layout);

    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    server.views = (struct wl_list){0};
    wl_list_init(&server.views);
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    #if HAS_XWAYLAND
    server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, true);
    if (server.xwayland) {
        wlr_log(WLR_INFO, "XWayland active");
        server.new_xwayland_surface.notify = server_new_xwayland_surface;
        wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
    }
    #endif

    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

    wl_list_init(&server.keyboards);

    init_ui(&server);

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    // 4. STARTUP CHECK
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend (Possible GPU/Input conflict)");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    wlr_log(WLR_INFO, "HACKERLAND ONLINE. Socket: %s", socket);
    wl_display_run(server.wl_display);

    // Cleanup
    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);
    return 0;
}
