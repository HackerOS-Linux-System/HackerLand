#include "server.h"
#include <stdlib.h>
#include <math.h>

// Linear Interpolation helper
static double lerp(double current, double target, double rate) {
    if (fabs(target - current) < 0.5) return target; // Snap if close
    return current + (target - current) * rate;
}

void update_view_animations(struct hk_server *server, long delta_time) {
    (void)delta_time; // Unused for now, could be used for frame-perfect anims
    struct hk_view *view;
    float rate = server->config.animation_speed;

    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped) continue;

        // Interpolate Geometry
        view->current.x = lerp(view->current.x, view->target.x, rate);
        view->current.y = lerp(view->current.y, view->target.y, rate);
        view->current.width = lerp(view->current.width, view->target.width, rate);
        view->current.height = lerp(view->current.height, view->target.height, rate);

        // Apply to Scene Graph
        wlr_scene_node_set_position(&view->scene_tree->node, (int)view->current.x, (int)view->current.y);

        // Update border geometry
        int bw = (int)server->config.border_width;
        wlr_scene_rect_set_size(view->border,
                                (int)view->current.width + bw * 2,
                                (int)view->current.height + bw * 2);
        wlr_scene_node_set_position(&view->border->node, -bw, -bw);

        // Tell the client to resize
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, (int)view->current.width, (int)view->current.height);
    }
}

void arrange_windows(struct hk_server *server) {
    if (wl_list_empty(&server->views)) return;

    // Output dimensions (Hardcoded for single monitor example)
    int screen_w = 1920;
    int screen_h = 1080;

    int gap = server->config.gap_size;
    int pad = server->config.padding;
    int bar_h = server->config.bar_height;

    int usable_w = screen_w - (2 * pad);
    int usable_h = screen_h - (2 * pad) - bar_h;
    int start_x = pad;
    int start_y = pad + bar_h;

    struct hk_view *view;
    int count = 0;
    wl_list_for_each(view, &server->views, link) {
        if (view->mapped) count++;
    }
    if (count == 0) return;

    // Master/Stack Logic
    int i = 0;
    int master_w = (count > 1) ? (usable_w / 2) - (gap / 2) : usable_w;
    int stack_w = usable_w - master_w - gap;
    int stack_h = (count > 1) ? (usable_h - (gap * (count - 2))) / (count - 1) : 0;

    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped) continue;

        // Set TARGET geometry
        if (i == 0) {
            view->target.x = start_x;
            view->target.y = start_y;
            view->target.width = master_w;
            view->target.height = usable_h;
            wlr_scene_rect_set_color(view->border, server->config.color_active_border);
        } else {
            view->target.x = start_x + master_w + gap;
            view->target.y = start_y + ((i - 1) * (stack_h + gap));
            view->target.width = stack_w;
            view->target.height = stack_h;
            wlr_scene_rect_set_color(view->border, server->config.color_inactive_border);
        }

        // Initialize current if it's new (assignment to struct allowed for named type)
        if (view->current.width == 0) {
            view->current = view->target;
            // Pop-in effect
            view->current.width = view->target.width * 0.8;
            view->current.height = view->target.height * 0.8;
            view->current.x = view->target.x + (view->target.width * 0.1);
            view->current.y = view->target.y + (view->target.height * 0.1);
        }

        i++;
    }
}
