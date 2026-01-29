#include "server.h"
#include <stdlib.h>
#include <math.h>
#include <wlr/util/log.h>

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct hk_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene *scene = output->server->scene;

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

    if (!scene_output) {
        return;
    }

    // --- ANIMATION & RENDER STEP ---
    update_view_animations(output->server, 16);

    // Commit the scene
    // If wlr_scene_output_commit returns true, it performed a swap.
    // If it returns false, no damage was present or it failed.
    if (!wlr_scene_output_commit(scene_output, NULL)) {
        // If we didn't swap, we won't get a pageflip event.
        // To prevent the loop from freezing if we *wanted* to animate something
        // but damage tracking failed, or just to be safe:
        // (In a production compositor, you only schedule if you have damage)
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct hk_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;

    wlr_log(WLR_INFO, "Output %s requested state change", output->wlr_output->name);
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct hk_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

void server_new_output(struct wl_listener *listener, void *data) {
    struct hk_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    // 1. Initialize Rendering
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct hk_output *output = calloc(1, sizeof(struct hk_output));
    output->wlr_output = wlr_output;
    output->server = server;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    // 3. Configure Layout
    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    // 4. MODE SELECTION (Fix for 1280x720 fallback)
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *best_mode = wlr_output_preferred_mode(wlr_output);

    // If no preferred mode, or if we want to force the "best" mode:
    // Iterate through modes to find the one with highest pixel count and refresh rate
    if (wl_list_empty(&wlr_output->modes)) {
        wlr_log(WLR_INFO, "Output %s has no modes (Custom DRM or Nested)", wlr_output->name);
    } else {
        struct wlr_output_mode *mode;
        wl_list_for_each(mode, &wlr_output->modes, link) {
            wlr_log(WLR_INFO, "Available mode: %dx%d @ %dHz", mode->width, mode->height, mode->refresh);
            if (!best_mode) {
                best_mode = mode;
                continue;
            }
            // Simple heuristic: wider is better, then higher refresh
            if (mode->width > best_mode->width) {
                best_mode = mode;
            } else if (mode->width == best_mode->width && mode->height > best_mode->height) {
                best_mode = mode;
            } else if (mode->width == best_mode->width && mode->height == best_mode->height && mode->refresh > best_mode->refresh) {
                best_mode = mode;
            }
        }
    }

    if (best_mode) {
        wlr_output_state_set_mode(&state, best_mode);
        wlr_log(WLR_INFO, "Output %s: Selected Best Mode %dx%d@%dHz",
                wlr_output->name, best_mode->width, best_mode->height, best_mode->refresh / 1000);
    }

    // 5. Adaptive Sync
    if (wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED) {
        wlr_output_state_set_adaptive_sync_enabled(&state, true);
    }

    // 6. Commit
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "Failed to commit output state for %s", wlr_output->name);
    }
    wlr_output_state_finish(&state);

    // KICKSTART: Force a frame immediately
    wlr_output_schedule_frame(wlr_output);
}
