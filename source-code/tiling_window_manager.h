#ifndef TILING_WINDOW_MANAGER_H
#define TILING_WINDOW_MANAGER_H

#include <miral/window_management_policy.h>
#include <miral/window_manager_tools.h>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>

// Struktura konfiguracji
struct Config {
    int gap_size = 10;
    int padding = 20;
    int bar_height = 42;
    double animation_speed = 0.15; // Szybsza domyślna animacja

    // Nowe pola wizualne
    int border_width = 2;
    int corner_radius = 8;
    std::string active_border_color = "#cba6f7";
    std::string inactive_border_color = "#585b70";

    std::string mode = "tiling"; // "tiling", "cage", "gamescope"
    std::string bar_position = "top";
    std::map<std::string, std::string> keybinds;
    bool enable_bar = true;
};

struct Geometry {
    double x, y, width, height;
};

class TilingWindowManager : public miral::WindowManagementPolicy {
public:
    TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config);
    ~TilingWindowManager();

    auto place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification override;
    void handle_window_ready(miral::WindowInfo& window_info) override;
    void advise_focus_gained(const miral::WindowInfo& window_info) override;
    void advise_focus_lost(const miral::WindowInfo& window_info) override;
    void advise_delete_window(const miral::WindowInfo& window_info) override;
    void handle_modify_window(miral::WindowInfo& window_info, const miral::WindowSpecification& modifications) override;
    void handle_raise_window(miral::WindowInfo& window_info) override;

    auto confirm_placement_on_display(const miral::WindowInfo& window_info, MirWindowState new_state, const mir::geometry::Rectangle& new_placement) -> mir::geometry::Rectangle override;
    auto confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle override;

    bool handle_keyboard_event(const MirKeyboardEvent* event) override;
    bool handle_touch_event(const MirTouchEvent* event) override;
    bool handle_pointer_event(const MirPointerEvent* event) override;

    void handle_request_move(miral::WindowInfo& window_info, const MirInputEvent* input_event) override;
    void handle_request_resize(miral::WindowInfo& window_info, const MirInputEvent* input_event, MirResizeEdge edge) override;

    void switch_workspace(int workspace_id);
    void reload_config(const Config& new_config);

private:
    miral::WindowManagerTools tools;
    Config config;
    std::recursive_mutex mutex;

    int current_workspace = 0;
    const int num_workspaces = 5;

    std::map<int, std::vector<miral::WindowInfo>> workspaces;
    std::vector<miral::WindowInfo> floating_windows;

    std::map<miral::Window, Geometry> currents;
    std::map<miral::Window, Geometry> targets;

    std::thread animation_thread;
    std::atomic<bool> running{true};

    void arrange_windows();
    void update_view_animations();
    void update_workspace_visibility();
    void update_ipc_file();
    double lerp(double a, double b, double f) { return a + f * (b - a); }
};

#endif // TILING_WINDOW_MANAGER_H
