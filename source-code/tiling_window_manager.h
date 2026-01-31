#ifndef TILING_WINDOW_MANAGER_H
#define TILING_WINDOW_MANAGER_H

#include <miral/window_management_policy.h>
#include <miral/window_manager_tools.h>
#include <miral/window_info.h>
#include <miral/window_specification.h>
#include <mir/geometry/rectangle.h>
#include <mir/geometry/point.h>
#include <mir/geometry/size.h>
#include <mir/geometry/displacement.h>
#include <mir_toolkit/events/event.h>
#include <mir_toolkit/events/input/input_event.h>
#include <mir_toolkit/events/input/keyboard_event.h>
#include <mir_toolkit/events/input/touch_event.h>
#include <mir_toolkit/events/input/pointer_event.h>
// FIX: Removed missing header <mir_toolkit/events/input_event_modifier.h>
// The modifier definitions are usually available via input_event.h or event.h in newer Mir versions.

#include <vector>
#include <map>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <mutex>
#include <fstream>

struct Config {
    int gap_size = 12;
    int padding = 12;
    int bar_height = 36;
    float border_width = 3.0f;
    float active_opacity = 0.98f;
    float inactive_opacity = 0.85f;
    float animation_speed = 0.12f;
    float color_active_border[4] = {0.0f, 0.9f, 1.0f, 1.0f};
    float color_inactive_border[4] = {0.3f, 0.0f, 0.5f, 1.0f};
};

struct Geometry {
    double x, y, width, height;
};

class TilingWindowManager : public miral::WindowManagementPolicy {
public:
    TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config);
    ~TilingWindowManager() override;

    auto place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification override;
    void handle_window_ready(miral::WindowInfo& window_info) override;
    void advise_focus_gained(const miral::WindowInfo& window_info) override;
    void advise_focus_lost(const miral::WindowInfo& window_info) override;
    void advise_delete_window(const miral::WindowInfo& window_info) override;

    void handle_modify_window(miral::WindowInfo& window_info, const miral::WindowSpecification& modifications) override;
    void handle_raise_window(miral::WindowInfo& window_info) override;
    auto confirm_placement_on_display(const miral::WindowInfo& window_info, MirWindowState new_state, const mir::geometry::Rectangle& new_placement) -> mir::geometry::Rectangle override;
    bool handle_keyboard_event(const MirKeyboardEvent* event) override;
    bool handle_touch_event(const MirTouchEvent* event) override;
    bool handle_pointer_event(const MirPointerEvent* event) override;
    void handle_request_move(miral::WindowInfo& window_info, const MirInputEvent* input_event) override;
    void handle_request_resize(miral::WindowInfo& window_info, const MirInputEvent* input_event, MirResizeEdge edge) override;
    auto confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle override;

    void switch_workspace(int workspace_id);

private:
    static double lerp(double current, double target, double rate) {
        if (std::fabs(target - current) < 0.5) return target;
        return current + (target - current) * rate;
    }

    void arrange_windows();
    void update_view_animations();
    void update_workspace_visibility();
    void update_ipc_file();

    miral::WindowManagerTools tools;
    Config config;
    std::map<int, std::vector<miral::WindowInfo>> workspaces;
    std::vector<miral::WindowInfo> floating_windows;
    std::map<miral::Window, Geometry> currents;
    std::map<miral::Window, Geometry> targets;
    int current_workspace = 0;
    int num_workspaces = 5;  // Konfigurowalna liczba pulpitów

    std::atomic<bool> running{true};
    std::thread animation_thread;
    std::recursive_mutex mutex;
};

#endif

