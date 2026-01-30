#ifndef TILING_WINDOW_MANAGER_H
#define TILING_WINDOW_MANAGER_H

#include <miral/window_management_policy.h>
#include <miral/window_manager_tools.h>
#include <miral/window_info.h>
#include <mir/geometry/rectangle.h>
#include <mir/geometry/point.h>
#include <mir/geometry/size.h>

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

    auto place_new_window(const miral::ApplicationInfo& app_info, const mir::geometry::Rectangle& requested) -> mir::geometry::Rectangle override;
    void handle_window_ready(miral::WindowInfo& window_info) override;
    void advise_focus_gained(const miral::WindowInfo& window_info) override;
    void advise_focus_lost(const miral::WindowInfo& window_info) override;
    void advise_delete_window(const miral::WindowInfo& window_info) override;

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
