#include "tiling_window_manager.h"

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
    : tools(tools), config(config) {
    animation_thread = std::thread{[this] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            tools.invoke_under_lock([this] {
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

auto TilingWindowManager::place_new_window(const miral::ApplicationInfo& app_info, const mir::geometry::Rectangle& requested) -> mir::geometry::Rectangle {
    return requested;  // Początkowe umieszczenie; układanie w handle_window_ready
}

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Sprawdź, czy to okno pływające (floating)
    if (window_info.type() == mir_window_type_dialog || window_info.type() == mir_window_type_utility) {
        floating_windows.push_back(window_info);
        // Pływające okna są zawsze widoczne, bez animacji/kafelek
        tools.force_visible(window_info.window());
        tools.raise_window(window_info.window());  // Na wierzchu
        return;
    }

    // Dodaj do aktualnego workspace'a
    if (workspaces.find(current_workspace) == workspaces.end()) {
        workspaces[current_workspace] = {};
    }
    workspaces[current_workspace].push_back(window_info);
    arrange_windows();
    update_workspace_visibility();
}

void TilingWindowManager::advise_focus_gained(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    tools.set_opacity(const_cast<miral::WindowInfo&>(window_info), config.active_opacity);
    // Kolory borderów nie obsługiwane bezpośrednio; użyj opacity do rozróżnienia
}

void TilingWindowManager::advise_focus_lost(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    tools.set_opacity(const_cast<miral::WindowInfo&>(window_info), config.inactive_opacity);
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

void TilingWindowManager::switch_workspace(int workspace_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (workspace_id < 0 || workspace_id >= num_workspaces) return;
    current_workspace = workspace_id;
    arrange_windows();
    update_workspace_visibility();
    update_ipc_file();
}

void TilingWindowManager::arrange_windows() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto area = tools.active_display_area();
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
        tools.force_visible(float_view.window());
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

            mir::geometry::Rectangle rect{
                {mir::geometry::X(static_cast<int>(cur.x)), mir::geometry::Y(static_cast<int>(cur.y))},
                {mir::geometry::Width(static_cast<int>(cur.width)), mir::geometry::Height(static_cast<int>(cur.height))}
            };
            tools.place_and_size_window(view, rect);
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
                tools.force_visible(view.window());
            }
        } else {
            for (auto& view : ws.second) {
                tools.force_not_visible(view.window());
            }
        }
    }

    // Pływające zawsze widoczne
    for (auto& float_view : floating_windows) {
        tools.force_visible(float_view.window());
    }
}

void TilingWindowManager::update_ipc_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::ofstream ipc_file("/tmp/hackerland_state");
    if (ipc_file.is_open()) {
        ipc_file << "current_workspace: " << current_workspace << "\n";
        ipc_file << "num_workspaces: " << num_workspaces << "\n";
        ipc_file << "windows_in_current: " << workspaces[current_workspace].size() << "\n";
        ipc_file.close();
    }
}
