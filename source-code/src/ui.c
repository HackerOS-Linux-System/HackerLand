#include "server.h"

// Initialize the HackerLand UI
void init_ui(struct hk_server *server) {
    // 1. Background - Deep, dark cyberpunk void
    float color_bg[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    struct wlr_scene_rect *bg = wlr_scene_rect_create(&server->scene->tree, 4000, 4000, color_bg);
    wlr_scene_node_set_position(&bg->node, 0, 0);
    wlr_scene_node_lower_to_bottom(&bg->node);

    // 2. "Cyber Grid" - Faint, high-tech grid
    struct wlr_scene_tree *grid_tree = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_set_position(&grid_tree->node, 0, 0);

    float color_grid_major[4] = {0.15f, 0.15f, 0.25f, 1.0f};
    float color_grid_minor[4] = {0.08f, 0.08f, 0.12f, 1.0f};

    // Minor Grid (every 50px)
    for (int x = 0; x < 3840; x += 50) {
        struct wlr_scene_rect *line = wlr_scene_rect_create(grid_tree, 1, 2160, color_grid_minor);
        wlr_scene_node_set_position(&line->node, x, 0);
    }
    for (int y = 0; y < 2160; y += 50) {
        struct wlr_scene_rect *line = wlr_scene_rect_create(grid_tree, 3840, 1, color_grid_minor);
        wlr_scene_node_set_position(&line->node, 0, y);
    }

    // Major Grid (every 250px)
    for (int x = 0; x < 3840; x += 250) {
        struct wlr_scene_rect *line = wlr_scene_rect_create(grid_tree, 2, 2160, color_grid_major);
        wlr_scene_node_set_position(&line->node, x, 0);
    }
    for (int y = 0; y < 2160; y += 250) {
        struct wlr_scene_rect *line = wlr_scene_rect_create(grid_tree, 3840, 2, color_grid_major);
        wlr_scene_node_set_position(&line->node, 0, y);
    }

    // 3. UI Layer (Top Bar)
    struct wlr_scene_tree *ui_tree = wlr_scene_tree_create(&server->scene->tree);

    int screen_w = 3840;
    int bar_h = server->config.bar_height;

    // Glassmorphism Bar Background
    float color_bar[4] = {0.02f, 0.02f, 0.04f, 0.95f};
    struct wlr_scene_rect *bar = wlr_scene_rect_create(ui_tree, screen_w, bar_h, color_bar);
    wlr_scene_node_set_position(&bar->node, 0, 0);

    // Neon Accent Line (Bottom of bar)
    float color_neon[4] = {0.0f, 0.9f, 1.0f, 1.0f}; // Cyan
    struct wlr_scene_rect *bar_border = wlr_scene_rect_create(ui_tree, screen_w, 2, color_neon);
    wlr_scene_node_set_position(&bar_border->node, 0, bar_h - 2);

    // "HACKERLAND" Badge Placeholder (Left)
    // Represented by a solid block for now, typically you'd render text textures here
    float color_badge[4] = {0.2f, 0.0f, 0.4f, 1.0f};
    struct wlr_scene_rect *badge = wlr_scene_rect_create(ui_tree, 140, bar_h - 10, color_badge);
    wlr_scene_node_set_position(&badge->node, 10, 5);

    // Status Indicators (Right)
    float color_active[4] = {0.0f, 1.0f, 0.5f, 1.0f}; // Green
    float color_busy[4] = {1.0f, 0.2f, 0.2f, 1.0f};   // Red

    struct wlr_scene_rect *stat1 = wlr_scene_rect_create(ui_tree, 8, 8, color_active);
    wlr_scene_node_set_position(&stat1->node, 1920 - 20, 14);

    struct wlr_scene_rect *stat2 = wlr_scene_rect_create(ui_tree, 8, 8, color_busy);
    wlr_scene_node_set_position(&stat2->node, 1920 - 40, 14);
}
