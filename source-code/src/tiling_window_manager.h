#ifndef TILING_WINDOW_MANAGER_H
#define TILING_WINDOW_MANAGER_H

#include <mir_toolkit/events/event.h>
#include <miral/window_management_policy.h>
#include <miral/window_manager_tools.h>
#include <miral/application_info.h>
#include <mir_toolkit/events/input/input_event.h>

#include "config.h"
#include "ipc_server.h"

#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <set>

enum class Layout { MasterStack, Monocle, Grid };

struct AnimState {
    double val, target, velocity;
};

struct WindowAnimState {
    AnimState x, y, w, h, scale, alpha;

    void force(double tx, double ty, double tw, double th) {
        x.val = x.target = tx; x.velocity = 0;
        y.val = y.target = ty; y.velocity = 0;
        w.val = w.target = tw; w.velocity = 0;
        h.val = h.target = th; h.velocity = 0;
        scale.val = scale.target = 0.5; scale.velocity = 0; // Start zoomed out
        alpha.val = alpha.target = 0.0; alpha.velocity = 0; // Start invisible
    }

    void set_target(double tx, double ty, double tw, double th, double tscale, double talpha) {
        x.target = tx; y.target = ty; w.target = tw; h.target = th;
        scale.target = tscale; alpha.target = talpha;
    }
};

class TilingWindowManager : public miral::WindowManagementPolicy {
public:
    TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config);
    ~TilingWindowManager();

    // API
    void reload_config(const Config& new_config);
    void switch_workspace(int id);
    void toggle_scratchpad();
    void toggle_sticky();
    void cycle_layout();
    void resize_master(double delta);

    // Miral Overrides
    auto place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification override;
    void handle_window_ready(miral::WindowInfo& window_info) override;
    void advise_focus_gained(const miral::WindowInfo& window_info) override;
    void advise_delete_window(const miral::WindowInfo& window_info) override;
    void handle_modify_window(miral::WindowInfo& window_info, const miral::WindowSpecification& modifications) override;
    void handle_raise_window(miral::WindowInfo& window_info) override;

    // Interactions
    bool handle_pointer_event(const MirPointerEvent* event) override;
    bool handle_keyboard_event(const MirKeyboardEvent* event) override { return false; }
    bool handle_touch_event(const MirTouchEvent* event) override { return false; }

    auto confirm_placement_on_display(const miral::WindowInfo& window_info, MirWindowState new_state, const mir::geometry::Rectangle& new_placement) -> mir::geometry::Rectangle override;
    auto confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle override;

    void handle_request_move(miral::WindowInfo& window_info, const MirInputEvent* input_event) override;
    void handle_request_resize(miral::WindowInfo& window_info, const MirInputEvent* input_event, MirResizeEdge edge) override;

    // Manual focus logic
    void focus_follows_mouse(const mir::geometry::Point& cursor);

private:
    miral::WindowManagerTools tools;
    Config config;
    IPCServer ipc;
    std::recursive_mutex mutex;

    // State
    int current_workspace = 0;
    Layout current_layout = Layout::MasterStack;
    double master_split = 0.5;
    miral::Window active_window;

    // Window Lists - Storing Window instead of WindowInfo
    std::map<int, std::vector<miral::Window>> workspaces;
    std::vector<miral::Window> floating_windows;
    std::vector<miral::Window> scratchpad_windows;
    std::set<miral::Window> sticky_windows;

    // Animation
    std::map<miral::Window, WindowAnimState> anim_states;
    std::thread anim_thread;
    std::atomic<bool> running{true};

    // Mouse Interaction
    struct DragState {
        bool active = false;
        miral::WindowInfo window; // WindowInfo is okay here as a snapshot for dragging logic
        int start_x, start_y;
        int win_start_x, win_start_y, win_start_w, win_start_h;
        enum { None, Move, Resize } mode = None;
    } drag_state;

    // Logic
    void arrange_windows();
    void step_physics(double dt);
    void broadcast_state();
};

#endif // TILING_WINDOW_MANAGER_H
