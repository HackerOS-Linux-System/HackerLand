#include "tiling_window_manager.h"
#include <iostream>
#include <algorithm>

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
: tools(tools), config(config) {
    update_ipc_file();

    animation_thread = std::thread{[this] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 100 FPS dla płynności
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

void TilingWindowManager::reload_config(const Config& new_config) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    config = new_config;
    arrange_windows();
}

auto TilingWindowManager::place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Dialogi i menu zawsze swobodne
    if (requested_specification.type() == mir_window_type_dialog ||
        requested_specification.type() == mir_window_type_utility ||
        requested_specification.type() == mir_window_type_menu ||
        requested_specification.type() == mir_window_type_tip) {
        return requested_specification;
        }

        // Pasek i tło
        if (requested_specification.name().is_set()) {
            std::string name = requested_specification.name().value();
            if (name.find("hackerbar") != std::string::npos) {
                miral::WindowSpecification spec = requested_specification;
                if (config.bar_position == "bottom") {
                    spec.top_left() = mir::geometry::Point{0, 1080 - config.bar_height};
                } else {
                    spec.top_left() = mir::geometry::Point{0, 0};
                }
                return spec;
            }
        }

        // Domyślna specyfikacja dla nowych okien
        // Ustawiamy je poza ekranem lub na środku, ale arrange_windows zaraz to poprawi
        miral::WindowSpecification spec = requested_specification;
        spec.size() = mir::geometry::Size{800, 600};
        spec.state() = mir_window_state_restored;
        return spec;
}

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto type = window_info.type();
    std::string name = window_info.name();

    // Ignoruj okna systemowe/specjalne
    if (name.find("hackerbar") != std::string::npos ||
        name.find("hackerland-bg") != std::string::npos ||
        type == mir_window_type_menu ||
        type == mir_window_type_satellite ||
        type == mir_window_type_tip ||
        type == mir_window_type_freestyle ||
        type == mir_window_type_inputmethod) {

        miral::WindowSpecification spec;
    spec.state() = mir_window_state_restored;
    tools.modify_window(window_info, spec);
    tools.raise_tree(window_info.window());
    return;
        }

        // Okna pływające
        if (type == mir_window_type_dialog || type == mir_window_type_utility) {
            floating_windows.push_back(window_info);
            miral::WindowSpecification spec;
            spec.state() = mir_window_state_restored;
            tools.modify_window(window_info, spec);
            tools.raise_tree(window_info.window());
            return;
        }

        // Sprawdź czy okno już jest w workspace (unikaj duplikatów)
        if (workspaces.find(current_workspace) == workspaces.end()) {
            workspaces[current_workspace] = {};
        }

        auto& ws_windows = workspaces[current_workspace];
        bool exists = false;
        for(const auto& w : ws_windows) {
            if (w.window() == window_info.window()) exists = true;
        }

        if (!exists) {
            workspaces[current_workspace].push_back(window_info);

            // Startowa geometria dla animacji "Pop-in"
            // Startujemy ze środka ekranu
            mir::geometry::Rectangle area{{0, 0}, {1920, 1080}};
            try { area = tools.active_output(); } catch (...) {}

            Geometry start_geo;
            start_geo.width = area.size.width.as_int() * 0.5;
            start_geo.height = area.size.height.as_int() * 0.5;
            start_geo.x = area.top_left.x.as_int() + (area.size.width.as_int() - start_geo.width) / 2;
            start_geo.y = area.top_left.y.as_int() + (area.size.height.as_int() - start_geo.height) / 2;

            currents[window_info.window()] = start_geo;
        }

        miral::WindowSpecification spec;
        spec.state() = mir_window_state_restored;
        tools.modify_window(window_info, spec);
        tools.raise_tree(window_info.window());

        // Przelicz układ od razu!
        arrange_windows();
        update_workspace_visibility();
        update_ipc_file();
}

void TilingWindowManager::advise_focus_gained(const miral::WindowInfo& window_info) {
}

void TilingWindowManager::advise_focus_lost(const miral::WindowInfo& window_info) {
}

void TilingWindowManager::advise_delete_window(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto float_it = std::find_if(floating_windows.begin(), floating_windows.end(), [&](const miral::WindowInfo& w) {
        return w.window() == window_info.window();
    });
    if (float_it != floating_windows.end()) {
        floating_windows.erase(float_it);
        return;
    }

    bool changed = false;
    for (auto& ws : workspaces) {
        auto it = std::find_if(ws.second.begin(), ws.second.end(), [&](const miral::WindowInfo& w) {
            return w.window() == window_info.window();
        });
        if (it != ws.second.end()) {
            ws.second.erase(it);
            changed = true;
            currents.erase(window_info.window());
            targets.erase(window_info.window());
        }
    }

    if (changed) {
        arrange_windows();
        update_workspace_visibility();
        update_ipc_file();
    }
}

void TilingWindowManager::handle_modify_window(miral::WindowInfo& window_info, const miral::WindowSpecification& modifications) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    tools.modify_window(window_info, modifications);
}

void TilingWindowManager::handle_raise_window(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
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
}

void TilingWindowManager::handle_request_resize(miral::WindowInfo& window_info, const MirInputEvent* input_event, MirResizeEdge edge) {
}

auto TilingWindowManager::confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle {
    return {window_info.window().top_left() + movement, window_info.window().size()};
}

void TilingWindowManager::switch_workspace(int workspace_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (workspace_id < 0 || workspace_id >= num_workspaces) return;

    if (current_workspace != workspace_id) {
        current_workspace = workspace_id;
        if (workspaces.find(current_workspace) == workspaces.end()) {
            workspaces[current_workspace] = {};
        }
        arrange_windows();
        update_workspace_visibility();
        update_ipc_file();
    }
}

void TilingWindowManager::arrange_windows() {
    mir::geometry::Rectangle area{{0, 0}, {1920, 1080}};
    try {
        auto rect = tools.active_output();
        if (rect.size.width.as_int() > 0) area = rect;
    } catch (...) {}

    if (config.mode == "cage" || config.mode == "gamescope") {
        auto& views = workspaces[current_workspace];
        for (auto& view : views) {
            Geometry tar;
            tar.x = area.top_left.x.as_int();
            tar.y = area.top_left.y.as_int();
            tar.width = area.size.width.as_int();
            tar.height = area.size.height.as_int();

            targets[view.window()] = tar;
            if (currents.find(view.window()) == currents.end()) currents[view.window()] = tar;
        }
        return;
    }

    int screen_w = area.size.width.as_int();
    int screen_h = area.size.height.as_int();
    int gap = config.gap_size;
    int pad = config.padding;
    int bar_h = config.bar_height;
    int bw = config.border_width;
    int bar_y_offset = (config.bar_position == "top") ? bar_h : 0;

    int usable_w = screen_w - 2 * pad;
    int usable_h = screen_h - 2 * pad - bar_h;
    int start_x = area.top_left.x.as_int() + pad;
    int start_y = area.top_left.y.as_int() + pad + bar_y_offset;

    auto& views = workspaces[current_workspace];
    size_t count = views.size();

    if (count == 0) return;

    // Algorytm Master + Stack
    // Okno 0: Master (Lewa strona)
    // Okna 1..n: Stack (Prawa strona, dzielona w pionie)

    int master_count = 1; // Zawsze 1 master
    int stack_count = count - master_count;

    int master_w = (stack_count > 0) ? (usable_w / 2 - gap / 2) : usable_w;
    int stack_w = usable_w - master_w - gap;

    int stack_h_per_window = (stack_count > 0) ? ((usable_h - (gap * (stack_count - 1))) / stack_count) : 0;

    int i = 0;
    for (auto& view : views) {
        auto win = view.window();
        Geometry tar;

        if (i < master_count) {
            // Master Window
            tar.x = start_x;
            tar.y = start_y;
            tar.width = master_w;
            tar.height = usable_h;
        } else {
            // Stack Window(s)
            int stack_idx = i - master_count;
            tar.x = start_x + master_w + gap;
            tar.y = start_y + stack_idx * (stack_h_per_window + gap);
            tar.width = stack_w;
            tar.height = stack_h_per_window;

            // Poprawka dla ostatniego okna (zaokrąglenia)
            if (stack_idx == stack_count - 1) {
                tar.height = (start_y + usable_h) - tar.y;
            }
        }

        // Apply borders (margin)
        if (bw > 0) {
            tar.x += bw;
            tar.y += bw;
            tar.width = std::max(1.0, tar.width - 2 * bw);
            tar.height = std::max(1.0, tar.height - 2 * bw);
        }

        targets[win] = tar;
        if (currents.find(win) == currents.end()) {
            currents[win] = tar; // Brak animacji dla "zaginionych" okien
        }
        i++;
    }
}

void TilingWindowManager::update_view_animations() {
    auto& views = workspaces[current_workspace];
    for (auto& view : views) {
        auto win = view.window();

        // Safety checks
        if (currents.find(win) == currents.end()) continue;
        if (targets.find(win) == targets.end()) continue;

        auto& cur = currents[win];
        auto& tar = targets[win];

        double speed = (config.mode == "gamescope") ? 1.0 : config.animation_speed;

        double new_x = lerp(cur.x, tar.x, speed);
        double new_y = lerp(cur.y, tar.y, speed);
        double new_w = lerp(cur.width, tar.width, speed);
        double new_h = lerp(cur.height, tar.height, speed);

        // Snap to finish
        if (std::abs(new_x - tar.x) < 1.0) new_x = tar.x;
        if (std::abs(new_y - tar.y) < 1.0) new_y = tar.y;
        if (std::abs(new_w - tar.width) < 1.0) new_w = tar.width;
        if (std::abs(new_h - tar.height) < 1.0) new_h = tar.height;

        bool changed = (cur.x != new_x || cur.y != new_y || cur.width != new_w || cur.height != new_h);

        cur.x = new_x;
        cur.y = new_y;
        cur.width = new_w;
        cur.height = new_h;

        if (changed) {
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
}

void TilingWindowManager::update_workspace_visibility() {
    for (auto& ws : workspaces) {
        bool is_current = (ws.first == current_workspace);
        for (auto& view : ws.second) {
            miral::WindowSpecification spec;
            if (is_current) {
                spec.state() = mir_window_state_restored;
            } else {
                spec.state() = mir_window_state_hidden;
            }
            tools.modify_window(view, spec);
        }
    }
}

void TilingWindowManager::update_ipc_file() {
    std::ofstream ipc_file("/tmp/hackerland_state", std::ios::trunc);
    if (ipc_file.is_open()) {
        size_t win_count = (workspaces.find(current_workspace) != workspaces.end()) ? workspaces[current_workspace].size() : 0;
        ipc_file << "workspace=" << current_workspace + 1 << "\n";
        ipc_file << "windows=" << win_count << "\n";
        ipc_file.close();
    }
}
