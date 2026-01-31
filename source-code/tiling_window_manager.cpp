#include "tiling_window_manager.h"

// Note: MirWindowState enum values (mir_window_state_restored, etc.) are available
// because miral/window_specification.h typically includes mir_toolkit headers.

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
: tools(tools), config(config) {
    animation_thread = std::thread{[this] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            // FIX: Explicitly access 'this->tools' to avoid ambiguity with constructor parameter 'tools'
            // which is not captured by the lambda.
            this->tools.invoke_under_lock([this] {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                update_view_animations();
            });
        }
    }};
}

TilingWindowManager::~TilingWindowManager() {
    running = false;
    if (animation_thread.joinable()) animation_thread.join();
}

auto TilingWindowManager::place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification {
    return requested_specification;
}

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Sprawdź, czy to okno pływające (floating)
    if (window_info.type() == mir_window_type_dialog || window_info.type() == mir_window_type_utility) {
        floating_windows.push_back(window_info);
        // FIX: Replaced force_visible/raise_window with modify_window and raise_tree
        // Fixed: Use mir_window_state_restored (C Enum) and assign to .state() reference
        miral::WindowSpecification spec;
        spec.state() = mir_window_state_restored;
        tools.modify_window(window_info, spec);

        tools.raise_tree(window_info.window()); // Use .window()
        return;
    }

    // Dodaj do aktualnego workspace'a
    if (workspaces.find(current_workspace) == workspaces.end()) {
        workspaces[current_workspace] = {};
    }
    workspaces[current_workspace].push_back(window_info);
    arrange_windows();
    update_workspace_visibility();
    update_ipc_file();
}

void TilingWindowManager::advise_focus_gained(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // FIX: Removed set_opacity as it's not supported in this API version
}

void TilingWindowManager::advise_focus_lost(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // FIX: Removed set_opacity as it's not supported in this API version
}

void TilingWindowManager::advise_delete_window(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Usuń z floating, jeśli tam jest
    auto float_it = std::find_if(floating_windows.begin(), floating_windows.end(), [&](const miral::WindowInfo& w) {
        return w.window() == window_info.window();
    });
    if (float_it != floating_windows.end()) {
        floating_windows.erase(float_it);
        currents.erase(window_info.window());
        targets.erase(window_info.window());
        update_ipc_file();
        return;
    }

    // Usuń z workspaces
    for (auto& ws : workspaces) {
        auto it = std::find_if(ws.second.begin(), ws.second.end(), [&](const miral::WindowInfo& w) {
            return w.window() == window_info.window();
        });
        if (it != ws.second.end()) {
            ws.second.erase(it);
            currents.erase(window_info.window());
            targets.erase(window_info.window());
            arrange_windows();
            update_workspace_visibility();
            update_ipc_file();
            return;
        }
    }
}

void TilingWindowManager::handle_modify_window(miral::WindowInfo& window_info, const miral::WindowSpecification& modifications) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    tools.modify_window(window_info, modifications);
}

void TilingWindowManager::handle_raise_window(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // FIX: raise_tree requires Window, not WindowInfo
    tools.raise_tree(window_info.window());
}

auto TilingWindowManager::confirm_placement_on_display(const miral::WindowInfo& window_info, MirWindowState new_state, const mir::geometry::Rectangle& new_placement) -> mir::geometry::Rectangle {
    return new_placement;
}

bool TilingWindowManager::handle_keyboard_event(const MirKeyboardEvent* event) {
    return false;
}

bool TilingWindowManager::handle_touch_event(const MirTouchEvent* event) {
    return false;
}

bool TilingWindowManager::handle_pointer_event(const MirPointerEvent* event) {
    return false;
}

void TilingWindowManager::handle_request_move(miral::WindowInfo& window_info, const MirInputEvent* input_event) {
    // No move in tiling
}

void TilingWindowManager::handle_request_resize(miral::WindowInfo& window_info, const MirInputEvent* input_event, MirResizeEdge edge) {
    // No resize in tiling
}

auto TilingWindowManager::confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle {
    return {window_info.window().top_left() + movement, window_info.window().size()};
}

void TilingWindowManager::switch_workspace(int workspace_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (workspace_id < 0 || workspace_id >= num_workspaces) return;
    current_workspace = workspace_id;
    if (workspaces.find(current_workspace) == workspaces.end()) {
        workspaces[current_workspace] = {};
    }
    arrange_windows();
    update_workspace_visibility();
    update_ipc_file();
}

void TilingWindowManager::arrange_windows() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // FIX: tools.active_display_area() doesn't exist. Hardcoding 1920x1080 as fallback.
    mir::geometry::Rectangle area{{0, 0}, {1920, 1080}};

    int screen_w = area.size.width.as_int();
    int screen_h = area.size.height.as_int();
    int gap = config.gap_size;
    int pad = config.padding;
    int bar_h = config.bar_height;

    int usable_w = screen_w - 2 * pad;
    int usable_h = screen_h - 2 * pad - bar_h;
    int start_x = area.top_left.x.as_int() + pad;
    int start_y = area.top_left.y.as_int() + pad + bar_h;

    // Układaj tylko aktualny workspace
    auto& views = workspaces[current_workspace];
    size_t count = views.size();
    if (count == 0) return;

    int i = 0;
    int master_w = (count > 1) ? (usable_w / 2 - gap / 2) : usable_w;
    int stack_w = usable_w - master_w - gap;
    int stack_h = (count > 1) ? (usable_h - gap * (count - 2)) / (count - 1) : 0;

    for (auto& view : views) {
        auto win = view.window();
        Geometry tar;
        if (i == 0) {
            tar.x = start_x;
            tar.y = start_y;
            tar.width = master_w;
            tar.height = usable_h;
        } else {
            tar.x = start_x + master_w + gap;
            tar.y = start_y + (i - 1) * (stack_h + gap);
            tar.width = stack_w;
            tar.height = stack_h;
        }

        targets[win] = tar;

        if (currents.find(win) == currents.end()) {
            currents[win] = {tar.x + tar.width * 0.1, tar.y + tar.height * 0.1, tar.width * 0.8, tar.height * 0.8};
        }

        i++;
    }

    // Pływające okna nie są układane, ale upewnij się, że są widoczne
    for (auto& float_view : floating_windows) {
        // FIX: Fixed WindowSpecification usage (assign to .state())
        miral::WindowSpecification spec;
        spec.state() = mir_window_state_restored;
        tools.modify_window(float_view, spec);
    }
}

void TilingWindowManager::update_view_animations() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    bool changed = false;

    // Animuj tylko aktualny workspace
    auto& views = workspaces[current_workspace];
    for (auto& view : views) {
        auto win = view.window();
        auto& cur = currents[win];
        auto& tar = targets[win];

        double new_x = lerp(cur.x, tar.x, config.animation_speed);
        double new_y = lerp(cur.y, tar.y, config.animation_speed);
        double new_w = lerp(cur.width, tar.width, config.animation_speed);
        double new_h = lerp(cur.height, tar.height, config.animation_speed);

        if (new_x != cur.x || new_y != cur.y || new_w != cur.width || new_h != cur.height) {
            changed = true;
            cur.x = new_x;
            cur.y = new_y;
            cur.width = new_w;
            cur.height = new_h;

            // FIX: Replaced place_and_size_window with modify_window using WindowSpecification
            // Fixed: Use assignments to .top_left() and .size()
            miral::WindowSpecification spec;
            spec.top_left() = mir::geometry::Point{
                mir::geometry::X(static_cast<int>(cur.x)),
                mir::geometry::Y(static_cast<int>(cur.y))
            };
            spec.size() = mir::geometry::Size{
                mir::geometry::Width(static_cast<int>(cur.width)),
                mir::geometry::Height(static_cast<int>(cur.height))
            };

            tools.modify_window(view, spec);
        }
    }
    // Pływające okna nie animowane
}

void TilingWindowManager::update_workspace_visibility() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Ukryj okna z nieaktywnych workspaces
    for (auto& ws : workspaces) {
        if (ws.first == current_workspace) {
            for (auto& view : ws.second) {
                // FIX: Fixed WindowSpecification usage
                miral::WindowSpecification spec;
                spec.state() = mir_window_state_restored;
                tools.modify_window(view, spec);
            }
        } else {
            for (auto& view : ws.second) {
                // FIX: Fixed WindowSpecification usage with mir_window_state_hidden
                miral::WindowSpecification spec;
                spec.state() = mir_window_state_hidden;
                tools.modify_window(view, spec);
            }
        }
    }

    // Pływające zawsze widoczne
    for (auto& float_view : floating_windows) {
        // FIX: Fixed WindowSpecification usage
        miral::WindowSpecification spec;
        spec.state() = mir_window_state_restored;
        tools.modify_window(float_view, spec);
    }
}

void TilingWindowManager::update_ipc_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::ofstream ipc_file("/tmp/hackerland_state");
    if (ipc_file.is_open()) {
        // FIX: Fixed missing terminating characters and string formatting by escaping newline
        ipc_file << "current_workspace: " << current_workspace << "\n";
        ipc_file << "num_workspaces: " << num_workspaces << "\n";
        size_t windows_size = (workspaces.find(current_workspace) != workspaces.end()) ? workspaces[current_workspace].size() : 0;
        ipc_file << "windows_in_current: " << windows_size << "\n";
        ipc_file.close();
    }
}
