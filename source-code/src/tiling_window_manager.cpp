#include "tiling_window_manager.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
: tools(tools), config(config) {

    ipc.start();
    ipc.set_command_handler([this](const std::string& cmd) {
        if (cmd.find("switch ") == 0) {
            try { switch_workspace(std::stoi(cmd.substr(7)) - 1); } catch(...) {}
        } else if (cmd == "scratchpad") {
            toggle_scratchpad();
        } else if (cmd == "sticky") {
            toggle_sticky();
        }
    });

    anim_thread = std::thread([this] {
        auto last = std::chrono::steady_clock::now();
        while (running) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (dt > 0.1) dt = 0.1;

            this->tools.invoke_under_lock([this, dt] {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                step_physics(dt);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    });
}

TilingWindowManager::~TilingWindowManager() {
    running = false;
    if (anim_thread.joinable()) anim_thread.join();
    ipc.stop();
}

// --- API ---

void TilingWindowManager::reload_config(const Config& new_config) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    config = new_config;
    arrange_windows();
}

void TilingWindowManager::switch_workspace(int id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (id < 0 || id > 9 || id == current_workspace) return;
    current_workspace = id;
    arrange_windows();
    broadcast_state();
}

void TilingWindowManager::toggle_scratchpad() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!active_window) return;

    auto it = std::find(scratchpad_windows.begin(), scratchpad_windows.end(), active_window);

    if (it != scratchpad_windows.end()) {
        // Restore from scratchpad
        scratchpad_windows.erase(it);
        workspaces[current_workspace].push_back(active_window);
    } else {
        // Move to scratchpad
        // Remove from current workspace if exists
        auto& ws = workspaces[current_workspace];
        auto w_it = std::find(ws.begin(), ws.end(), active_window);
        if (w_it != ws.end()) ws.erase(w_it);

        scratchpad_windows.push_back(active_window);
    }
    arrange_windows();
    broadcast_state();
}

void TilingWindowManager::toggle_sticky() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!active_window) return;

    if (sticky_windows.count(active_window)) sticky_windows.erase(active_window);
    else sticky_windows.insert(active_window);

    broadcast_state();
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
    master_split += delta;
    if (master_split < 0.1) master_split = 0.1;
    if (master_split > 0.9) master_split = 0.9;
    arrange_windows();
}

// --- Layout & Physics ---

void TilingWindowManager::arrange_windows() {
    mir::geometry::Rectangle area = tools.active_output();

    int pad = config.padding;
    int gap = config.gap_size;
    int bar_h = config.enable_bar ? config.bar_height : 0;

    int start_x = area.top_left.x.as_int() + pad;
    int start_y = area.top_left.y.as_int() + pad + (config.bar_position == "top" ? bar_h : 0);
    int useful_w = area.size.width.as_int() - 2*pad;
    int useful_h = area.size.height.as_int() - 2*pad - bar_h;

    // Collect windows to tile
    std::vector<miral::Window> tiling_list;

    // 1. Sticky windows
    for (auto w : sticky_windows) {
        tiling_list.push_back(w);
    }

    // 2. Workspace windows (exclude sticky)
    for (auto& w : workspaces[current_workspace]) {
        if (sticky_windows.find(w) == sticky_windows.end()) {
            tiling_list.push_back(w);
        }
    }

    // Hide others (Scratchpad / Other Workspaces)
    for (auto& [id, list] : workspaces) {
        if (id == current_workspace) continue;
        for (auto& w : list) {
            if (anim_states.count(w)) {
                anim_states[w].set_target(0, 0, 0, 0, 0.5, 0.0); // Hide
            }
        }
    }
    for (auto& w : scratchpad_windows) {
        if (anim_states.count(w)) {
            anim_states[w].set_target(0, 0, 0, 0, 0.5, 0.0);
        }
    }

    // Calculate Rects
    size_t n = tiling_list.size();
    std::vector<mir::geometry::Rectangle> rects;

    if (n > 0) {
        if (current_layout == Layout::Monocle) {
            for (size_t i=0; i<n; ++i) rects.push_back({{start_x, start_y}, {useful_w, useful_h}});
        } else if (current_layout == Layout::Grid) {
            int cols = std::ceil(std::sqrt(n));
            int rows = std::ceil((double)n / cols);
            int cw = (useful_w - gap*(cols-1))/cols;
            int ch = (useful_h - gap*(rows-1))/rows;
            for(size_t i=0; i<n; ++i) {
                int r = i/cols; int c = i%cols;
                rects.push_back({{start_x + c*(cw+gap), start_y + r*(ch+gap)}, {cw, ch}});
            }
        } else {
            // MasterStack
            if (n == 1) {
                rects.push_back({{start_x, start_y}, {useful_w, useful_h}});
            } else {
                int mw = useful_w * master_split;
                int sw = useful_w - mw - gap;
                int sh_h = (useful_h - gap*(n-2)) / (n-1);

                rects.push_back({{start_x, start_y}, {mw, useful_h}});
                for(size_t i=1; i<n; ++i) {
                    rects.push_back({{start_x + mw + gap, start_y + (int)(i-1)*(sh_h+gap)}, {sw, sh_h}});
                }
            }
        }
    }

    // Apply targets
    for(size_t i=0; i<n; ++i) {
        auto win = tiling_list[i];
        if (anim_states.find(win) == anim_states.end()) {
            WindowAnimState s;
            mir::geometry::Rectangle r = rects[i];
            s.force(r.top_left.x.as_int(), r.top_left.y.as_int(), r.size.width.as_int(), r.size.height.as_int());
            anim_states[win] = s;
        }

        auto& st = anim_states[win];
        auto r = rects[i];

        int bw = config.border_width;
        st.set_target(
            r.top_left.x.as_int() + bw,
                      r.top_left.y.as_int() + bw,
                      r.size.width.as_int() - 2*bw,
                      r.size.height.as_int() - 2*bw,
                      1.0, 1.0
        );
    }
}

void TilingWindowManager::step_physics(double dt) {
    auto step = [&](AnimState& s) {
        double force = -config.spring_tension * (s.val - s.target) - config.spring_friction * s.velocity;
        s.velocity += force * dt;
        s.val += s.velocity * dt;
    };

    bool changes = false;
    for (auto it = anim_states.begin(); it != anim_states.end(); ) {
        auto win = it->first;
        auto& st = it->second;

        step(st.x); step(st.y); step(st.w); step(st.h); step(st.scale); step(st.alpha);

        try {
            miral::WindowSpecification spec;
            spec.top_left() = mir::geometry::Point{ (int)st.x.val, (int)st.y.val };
            spec.size() = mir::geometry::Size{ (int)st.w.val, (int)st.h.val };

            if (st.alpha.val < 0.1) spec.state() = mir_window_state_hidden;
            else if (st.alpha.target > 0.5) spec.state() = mir_window_state_restored;

            tools.modify_window(tools.info_for(win), spec);
            changes = true;
            ++it;
        } catch (...) {
            it = anim_states.erase(it);
        }
    }
}

// --- Events ---

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::string name = window_info.name();
    if (name.find("hackerbar") != std::string::npos || name.find("bg") != std::string::npos ||
        window_info.type() == mir_window_type_menu || window_info.type() == mir_window_type_tip) {
        tools.raise_tree(window_info.window());
    return;
        }

        if (window_info.type() == mir_window_type_dialog) {
            floating_windows.push_back(window_info.window());
        } else {
            workspaces[current_workspace].push_back(window_info.window());
        }

        // Init Animation
        auto out = tools.active_output();
        WindowAnimState st;
        int cx = out.top_left.x.as_int() + out.size.width.as_int()/2;
        int cy = out.top_left.y.as_int() + out.size.height.as_int()/2;
        st.force(cx-50, cy-50, 100, 100);
        anim_states[window_info.window()] = st;

        tools.raise_tree(window_info.window());
        arrange_windows();
        broadcast_state();
}

void TilingWindowManager::advise_delete_window(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto remove_from = [&](std::vector<miral::Window>& list) {
        auto it = std::find(list.begin(), list.end(), window_info.window());
        if (it != list.end()) list.erase(it);
    };

        for (auto& [id, list] : workspaces) remove_from(list);
        remove_from(floating_windows);
    remove_from(scratchpad_windows);
    if (sticky_windows.count(window_info.window())) sticky_windows.erase(window_info.window());

    anim_states.erase(window_info.window());

    arrange_windows();
    broadcast_state();
}

void TilingWindowManager::advise_focus_gained(const miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    active_window = window_info.window();
    broadcast_state();
}

// --- Interaction ---

void TilingWindowManager::focus_follows_mouse(const mir::geometry::Point& cursor) {
    // Check tiling windows via animation states (geometry source of truth)
    for (auto& pair : anim_states) {
        auto& st = pair.second;
        // Fix: Use double braces for Point and Size initialization
        mir::geometry::Rectangle r{{(int)st.x.val, (int)st.y.val}, {(int)st.w.val, (int)st.h.val}};
        if (r.contains(cursor)) {
            if (active_window != pair.first) {
                tools.raise_tree(pair.first);
            }
            return;
        }
    }
}

bool TilingWindowManager::handle_pointer_event(const MirPointerEvent* event) {
    // Focus follows mouse
    if (mir_pointer_event_action(event) == mir_pointer_action_motion) {
        mir::geometry::Point cursor{
            (int)mir_pointer_event_axis_value(event, mir_pointer_axis_x),
            (int)mir_pointer_event_axis_value(event, mir_pointer_axis_y)
        };
        focus_follows_mouse(cursor);
    }

    if (mir_pointer_event_action(event) == mir_pointer_action_button_down) {
        auto mods = mir_pointer_event_modifiers(event);
        bool alt = (mods & mir_input_event_modifier_alt);

        if (alt) {
            mir::geometry::Point cursor{
                (int)mir_pointer_event_axis_value(event, mir_pointer_axis_x),
                (int)mir_pointer_event_axis_value(event, mir_pointer_axis_y)
            };

            for (auto& pair : anim_states) {
                auto& st = pair.second;
                // Fix: Correct Rectangle initialization
                mir::geometry::Rectangle r{{(int)st.x.val, (int)st.y.val}, {(int)st.w.val, (int)st.h.val}};

                if (r.contains(cursor)) {
                    drag_state.active = true;
                    // We must snapshot info here for drag logic, info_for is safe here
                    try {
                        drag_state.window = tools.info_for(pair.first);
                        drag_state.start_x = cursor.x.as_int();
                        drag_state.start_y = cursor.y.as_int();
                        drag_state.win_start_x = st.x.val;
                        drag_state.win_start_y = st.y.val;
                        drag_state.win_start_w = st.w.val;
                        drag_state.win_start_h = st.h.val;

                        if (mir_pointer_event_button_state(event, mir_pointer_button_primary)) drag_state.mode = DragState::Move;
                        else if (mir_pointer_event_button_state(event, mir_pointer_button_secondary)) drag_state.mode = DragState::Resize;
                        return true;
                    } catch (...) {}
                }
            }
        }
    } else if (mir_pointer_event_action(event) == mir_pointer_action_button_up) {
        if (drag_state.active) {
            drag_state.active = false;
            return true;
        }
    } else if (mir_pointer_event_action(event) == mir_pointer_action_motion) {
        if (drag_state.active) {
            int cx = mir_pointer_event_axis_value(event, mir_pointer_axis_x);
            int cy = mir_pointer_event_axis_value(event, mir_pointer_axis_y);
            int dx = cx - drag_state.start_x;
            int dy = cy - drag_state.start_y;

            auto& st = anim_states[drag_state.window.window()];

            if (drag_state.mode == DragState::Move) {
                st.x.target = drag_state.win_start_x + dx;
                st.y.target = drag_state.win_start_y + dy;
            } else if (drag_state.mode == DragState::Resize) {
                st.w.target = std::max(50, drag_state.win_start_w + dx);
                st.h.target = std::max(50, drag_state.win_start_h + dy);
            }
            return true;
        }
    }
    return false;
}

void TilingWindowManager::broadcast_state() {
    std::stringstream ss;
    ss << "{"
    << "\"workspace\": " << current_workspace + 1 << ","
    << "\"layout\": \"" << (current_layout == Layout::Monocle ? "Monocle" : "Tiling") << "\","
    << "\"window_count\": " << workspaces[current_workspace].size() << ","
    << "\"sticky_active\": " << (sticky_windows.count(active_window) ? "true" : "false")
    << "}";
    ipc.broadcast(ss.str());
}

// Boilerplate
auto TilingWindowManager::place_new_window(const miral::ApplicationInfo&, const miral::WindowSpecification& s) -> miral::WindowSpecification {
    if (s.name().is_set() && s.name().value().find("hackerbar") != std::string::npos) {
        auto spec = s;
        spec.top_left() = mir::geometry::Point{0,0};
        return spec;
    }
    return s;
}
void TilingWindowManager::handle_modify_window(miral::WindowInfo& w, const miral::WindowSpecification& m) { tools.modify_window(w, m); }
void TilingWindowManager::handle_raise_window(miral::WindowInfo& w) { tools.raise_tree(w.window()); }
auto TilingWindowManager::confirm_placement_on_display(const miral::WindowInfo&, MirWindowState, const mir::geometry::Rectangle& r) -> mir::geometry::Rectangle { return r; }
auto TilingWindowManager::confirm_inherited_move(const miral::WindowInfo& w, mir::geometry::Displacement d) -> mir::geometry::Rectangle { return {w.window().top_left()+d, w.window().size()}; }
void TilingWindowManager::handle_request_move(miral::WindowInfo&, const MirInputEvent*) {}
void TilingWindowManager::handle_request_resize(miral::WindowInfo&, const MirInputEvent*, MirResizeEdge) {}
