#include "tiling_window_manager.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
: tools(tools), config(config) {

    setup_ipc();

    animation_thread = std::thread{[this] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    if (server_socket_fd != -1) {
        close(server_socket_fd);
        unlink("/tmp/hackerland.sock");
    }
    if (ipc_thread.joinable()) ipc_thread.join();
}

// --- IPC Implementation ---

void TilingWindowManager::setup_ipc() {
    ipc_thread = std::thread([this]() {
        const char* socket_path = "/tmp/hackerland.sock";
        unlink(socket_path);

        server_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_fd == -1) return;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

        if (bind(server_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) return;
        if (listen(server_socket_fd, 5) == -1) return;

        // Set non-blocking to handle accept/read in loop efficiently without blocking shutdown
        int flags = fcntl(server_socket_fd, F_GETFL, 0);
        fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK);

        while (running) {
            // Accept new clients
            int client_fd = accept(server_socket_fd, NULL, NULL);
            if (client_fd != -1) {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                client_sockets.insert(client_fd);
                // Send immediate state update to new client
                std::stringstream ss;
                size_t win_count = (workspaces.find(current_workspace) != workspaces.end()) ? workspaces[current_workspace].size() : 0;
                ss << "workspace:" << current_workspace + 1 << "|windows:" << win_count << "|title:" << active_window_title << "\n";
                std::string msg = ss.str();
                send(client_fd, msg.c_str(), msg.size(), MSG_NOSIGNAL);
            }

            // Read from clients (simple command processing)
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                for (auto it = client_sockets.begin(); it != client_sockets.end(); ) {
                    char buffer[256];
                    ssize_t bytes = recv(*it, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);

                    if (bytes > 0) {
                        buffer[bytes] = '\0';
    std::string cmd(buffer);
    if (cmd.find("switch ") == 0) {
        int ws = std::stoi(cmd.substr(7));
        if (ws >= 1 && ws <= 5) {
            int target = ws - 1;
            if (target != current_workspace) {
                current_workspace = target;
                if (workspaces.find(current_workspace) == workspaces.end()) {
                    workspaces[current_workspace] = {};
                }
            }
        }
    }
    ++it;
                    } else if (bytes == 0 || (bytes == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        close(*it);
                        it = client_sockets.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void TilingWindowManager::broadcast_state() {
    // Format: "workspace:1|windows:3|title:Alacritty\n"
    size_t win_count = (workspaces.find(current_workspace) != workspaces.end()) ? workspaces[current_workspace].size() : 0;

    std::stringstream ss;
    ss << "workspace:" << current_workspace + 1
    << "|windows:" << win_count
    << "|title:" << active_window_title
    << "\n";

    std::string msg = ss.str();

    for (auto it = client_sockets.begin(); it != client_sockets.end(); ) {
        ssize_t sent = send(*it, msg.c_str(), msg.size(), MSG_NOSIGNAL);
        if (sent == -1 && errno == EPIPE) {
            close(*it);
            it = client_sockets.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Logic ---

void TilingWindowManager::reload_config(const Config& new_config) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    config = new_config;
    arrange_windows();
}

auto TilingWindowManager::place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Dialogs & Utility
    if (requested_specification.type() == mir_window_type_dialog ||
        requested_specification.type() == mir_window_type_utility ||
        requested_specification.type() == mir_window_type_menu ||
        requested_specification.type() == mir_window_type_tip) {
        return requested_specification;
        }

        // Bar handling
        // Note: requested_specification.name() is Optional<string>
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

        miral::WindowSpecification spec = requested_specification;
        spec.size() = mir::geometry::Size{800, 600};
        spec.state() = mir_window_state_restored;
        return spec;
}

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto type = window_info.type();
    // Note: window_info.name() is std::string (direct)
    std::string name = window_info.name();

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

        if (type == mir_window_type_dialog || type == mir_window_type_utility) {
            floating_windows.push_back(window_info);
            miral::WindowSpecification spec;
            spec.state() = mir_window_state_restored;
            tools.modify_window(window_info, spec);
            tools.raise_tree(window_info.window());
            return;
        }

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

            // Pop-in animation origin
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

        arrange_windows();
        update_workspace_visibility();
        broadcast_state();
}

void TilingWindowManager::advise_focus_gained(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // FIX: window_info.name() returns std::string, not optional.
    active_window_title = window_info.name();
    if (active_window_title.empty()) {
        active_window_title = "Unknown";
    }
    broadcast_state();
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
        active_window_title = "Hackerland";
        arrange_windows();
        update_workspace_visibility();
        broadcast_state();
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

auto TilingWindowManager::confirm_inherited_move(const miral::WindowInfo& window_info, mir::geometry::Displacement movement) -> mir::geometry::Rectangle {
    return {window_info.window().top_left() + movement, window_info.window().size()};
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
        broadcast_state();
    }
}

void TilingWindowManager::cycle_layout() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current_layout == Layout::MasterStack) current_layout = Layout::Monocle;
    else if (current_layout == Layout::Monocle) current_layout = Layout::Grid;
    else current_layout = Layout::MasterStack;
    arrange_windows();
}

void TilingWindowManager::resize_master(double delta) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    master_split_ratio += delta;
    if (master_split_ratio < 0.1) master_split_ratio = 0.1;
    if (master_split_ratio > 0.9) master_split_ratio = 0.9;
    arrange_windows();
}

void TilingWindowManager::arrange_windows() {
    mir::geometry::Rectangle area{{0, 0}, {1920, 1080}};
    try {
        auto rect = tools.active_output();
        if (rect.size.width.as_int() > 0) area = rect;
    } catch (...) {}

    // Special game/cage modes override standard tiling
    if (config.mode == "cage" || config.mode == "gamescope") {
        current_layout = Layout::Monocle;
    }

    auto& views = workspaces[current_workspace];
    size_t count = views.size();
    if (count == 0) return;

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

    if (current_layout == Layout::Monocle) {
        for (auto& view : views) {
            Geometry tar;
            tar.x = start_x;
            tar.y = start_y;
            tar.width = usable_w;
            tar.height = usable_h;
            targets[view.window()] = tar;
            if (currents.find(view.window()) == currents.end()) currents[view.window()] = tar;
        }
        return;
    }

    if (current_layout == Layout::Grid) {
        int cols = std::ceil(std::sqrt(count));
        int rows = std::ceil((double)count / cols);

        int cell_w = (usable_w - (gap * (cols - 1))) / cols;
        int cell_h = (usable_h - (gap * (rows - 1))) / rows;

        int i = 0;
        for (auto& view : views) {
            int row = i / cols;
            int col = i % cols;

            Geometry tar;
            tar.x = start_x + col * (cell_w + gap);
            tar.y = start_y + row * (cell_h + gap);
            tar.width = cell_w;
            tar.height = cell_h;

            if (bw > 0) {
                tar.x += bw; tar.y += bw;
                tar.width -= 2*bw; tar.height -= 2*bw;
            }

            targets[view.window()] = tar;
            if (currents.find(view.window()) == currents.end()) currents[view.window()] = tar;
            i++;
        }
        return;
    }

    // Master + Stack (Default)
    int master_count = 1;
    int stack_count = count - master_count;

    int master_w = (stack_count > 0) ? (usable_w * master_split_ratio - gap / 2) : usable_w;
    int stack_w = usable_w - master_w - gap;

    int stack_h_per_window = (stack_count > 0) ? ((usable_h - (gap * (stack_count - 1))) / stack_count) : 0;

    int i = 0;
    for (auto& view : views) {
        auto win = view.window();
        Geometry tar;

        if (i < master_count) {
            tar.x = start_x;
            tar.y = start_y;
            tar.width = master_w;
            tar.height = usable_h;
        } else {
            int stack_idx = i - master_count;
            tar.x = start_x + master_w + gap;
            tar.y = start_y + stack_idx * (stack_h_per_window + gap);
            tar.width = stack_w;
            tar.height = stack_h_per_window;

            if (stack_idx == stack_count - 1) {
                tar.height = (start_y + usable_h) - tar.y;
            }
        }

        if (bw > 0) {
            tar.x += bw; tar.y += bw;
            tar.width = std::max(1.0, tar.width - 2 * bw);
            tar.height = std::max(1.0, tar.height - 2 * bw);
        }

        targets[win] = tar;
        if (currents.find(win) == currents.end()) currents[win] = tar;
        i++;
    }
}

void TilingWindowManager::update_view_animations() {
    auto& views = workspaces[current_workspace];
    for (auto& view : views) {
        auto win = view.window();
        if (currents.find(win) == currents.end()) continue;
        if (targets.find(win) == targets.end()) continue;

        auto& cur = currents[win];
        auto& tar = targets[win];

        double speed = (config.mode == "gamescope") ? 1.0 : config.animation_speed;

        double new_x = lerp(cur.x, tar.x, speed);
        double new_y = lerp(cur.y, tar.y, speed);
        double new_w = lerp(cur.width, tar.width, speed);
        double new_h = lerp(cur.height, tar.height, speed);

        if (std::abs(new_x - tar.x) < 0.5) new_x = tar.x;
        if (std::abs(new_y - tar.y) < 0.5) new_y = tar.y;
        if (std::abs(new_w - tar.width) < 0.5) new_w = tar.width;
        if (std::abs(new_h - tar.height) < 0.5) new_h = tar.height;

        bool changed = (cur.x != new_x || cur.y != new_y || cur.width != new_w || cur.height != new_h);

        cur.x = new_x; cur.y = new_y; cur.width = new_w; cur.height = new_h;

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
            if (is_current) spec.state() = mir_window_state_restored;
            else spec.state() = mir_window_state_hidden;
            tools.modify_window(view, spec);
        }
    }
}
