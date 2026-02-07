#include "tiling_window_manager.h"
#include <iostream>
#include <algorithm>

TilingWindowManager::TilingWindowManager(const miral::WindowManagerTools& tools, const Config& config)
: tools(tools), config(config) {
    update_ipc_file();

    animation_thread = std::thread{[this] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
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

// CRITICAL FIX: Zamiast zwracać requested_specification, obliczamy, gdzie okno powinno być
// i zwracamy tę pozycję. Dzięki temu nowe okno od razu "wskakuje" na swoje miejsce w kaflach
// zamiast pojawiać się jako małe okienko na środku.
auto TilingWindowManager::place_new_window(const miral::ApplicationInfo& app_info, const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Jeśli to okno dialogowe/narzędziowe, pozwól mu być gdzie chce (zazwyczaj środek)
    if (requested_specification.type() == mir_window_type_dialog ||
        requested_specification.type() == mir_window_type_utility ||
        requested_specification.type() == mir_window_type_menu) {
        return requested_specification;
        }

        // NAPRAWA POZYCJI PASKA: Jeśli nazwa to "hackerbar", wymuś pozycję (0,0)
        // Nawet jeśli Layer Shell nie zadziała poprawnie, to zapobiegnie centrowaniu paska.
        if (requested_specification.name().is_set() && requested_specification.name().value().find("hackerbar") != std::string::npos) {
            miral::WindowSpecification spec = requested_specification;
            spec.top_left() = mir::geometry::Point{0, 0};
            return spec;
        }

        // Dla normalnych okien aplikacji: oblicz przyszłą pozycję
        // Symulujemy dodanie okna do listy, żeby obliczyć geometrię, ale nie dodajemy go fizycznie jeszcze
        int current_count = 0;
        if (workspaces.find(current_workspace) != workspaces.end()) {
            current_count = workspaces[current_workspace].size();
        }

        // Będziemy mieli count + 1 okien
        int count = current_count + 1;

        // Pobierz wymiary ekranu
        mir::geometry::Rectangle area{{0, 0}, {1920, 1080}};
        try {
            auto rect = tools.active_output();
            if (rect.size.width.as_int() > 0) area = rect;
        } catch (...) {}

        int screen_w = area.size.width.as_int();
        int screen_h = area.size.height.as_int();
        int gap = config.gap_size;
        int pad = config.padding;
        int bar_h = config.bar_height;

        int usable_w = screen_w - 2 * pad;
        int usable_h = screen_h - 2 * pad - bar_h;
        int start_x = area.top_left.x.as_int() + pad;
        int start_y = area.top_left.y.as_int() + pad + bar_h;

        // Obliczamy geometrię dla NOWEGO okna (które będzie ostatnie w liście)
        mir::geometry::Rectangle target_rect;

        if (count == 1) {
            // Pierwsze okno - pełny ekran
            target_rect = {{start_x, start_y}, {usable_w, usable_h}};
        } else {
            // Kolejne okna idą na stos po prawej
            int master_w = usable_w / 2 - gap / 2;
            int stack_w = usable_w - master_w - gap;
            // Nowe okno ląduje na dole stosu
            // Uproszczenie: przy 2 oknach, drugie zajmuje całą prawą stronę
            // Przy >2 oknach, dzielimy prawą stronę.
            // Żeby nie komplikować logiki tutaj, po prostu zwróćmy geometrię "Master" lub "Stack Top"
            // Arrange windows i tak to poprawi w klatce animacji, ale ustawienie sensownego startu
            // zapobiega "miganiu" małego okna.

            target_rect = {{start_x + master_w + gap, start_y}, {stack_w, usable_h / (count - 1)}};
        }

        miral::WindowSpecification spec = requested_specification;
        spec.top_left() = target_rect.top_left;
        spec.size() = target_rect.size;
        spec.state() = mir_window_state_restored; // Wymuś stan restored, żeby nie było zmaksymalizowane "systemowo"

        return spec;
}

void TilingWindowManager::handle_window_ready(miral::WindowInfo& window_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    auto type = window_info.type();
    std::string name = window_info.name();

    if (name.find("hackerbar") != std::string::npos ||
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
        }

        // Upewnij się, że okno jest widoczne i w stanie restored
        miral::WindowSpecification spec;
        spec.state() = mir_window_state_restored;
        tools.modify_window(window_info, spec);
        tools.raise_tree(window_info.window());

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
    mir::geometry::Rectangle area{{0, 0}, {1280, 720}};

    try {
        auto rect = tools.active_output();
        if (rect.size.width.as_int() > 0 && rect.size.height.as_int() > 0) {
            area = rect;
        }
    } catch (...) {
    }

    int screen_w = area.size.width.as_int();
    int screen_h = area.size.height.as_int();

    if (screen_w <= 0) screen_w = 1280;
    if (screen_h <= 0) screen_h = 720;

    int gap = config.gap_size;
    int pad = config.padding;
    int bar_h = config.bar_height;

    int usable_w = screen_w - 2 * pad;
    int usable_h = screen_h - 2 * pad - bar_h;
    int start_x = area.top_left.x.as_int() + pad;
    int start_y = area.top_left.y.as_int() + pad + bar_h;

    auto& views = workspaces[current_workspace];
    size_t count = views.size();

    if (count == 0) return;

    int i = 0;
    int master_w = (count > 1) ? (usable_w / 2 - gap / 2) : usable_w;
    int stack_w = usable_w - master_w - gap;
    // Fix: jeśli mamy 2 okna, stack_h to pełna wysokość
    int stack_h = (count > 2) ? (usable_h - gap * (count - 2)) / (count - 1) : usable_h;

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
            // Ostatnie okno wypełnia resztę w pionie (fix na błędy zaokrągleń)
            if (i == count - 1) {
                tar.height = std::max(0, (start_y + usable_h) - static_cast<int>(tar.y));
            }
        }

        targets[win] = tar;

        if (currents.find(win) == currents.end()) {
            // Jeśli nowe okno, ustaw start na docelowej pozycji (bez animacji wlotu z nikąd)
            currents[win] = targets[win];
        }

        i++;
    }
}

void TilingWindowManager::update_view_animations() {
    auto& views = workspaces[current_workspace];
    for (auto& view : views) {
        auto win = view.window();
        auto& cur = currents[win];
        auto& tar = targets[win];

        double new_x = lerp(cur.x, tar.x, config.animation_speed);
        double new_y = lerp(cur.y, tar.y, config.animation_speed);
        double new_w = lerp(cur.width, tar.width, config.animation_speed);
        double new_h = lerp(cur.height, tar.height, config.animation_speed);

        if (std::abs(new_x - tar.x) < 0.5) new_x = tar.x;
        if (std::abs(new_y - tar.y) < 0.5) new_y = tar.y;
        if (std::abs(new_w - tar.width) < 0.5) new_w = tar.width;
        if (std::abs(new_h - tar.height) < 0.5) new_h = tar.height;

        if (new_x != cur.x || new_y != cur.y || new_w != cur.width || new_h != cur.height) {
            cur.x = new_x;
            cur.y = new_y;
            cur.width = new_w;
            cur.height = new_h;

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
            spec.state() = is_current ? mir_window_state_restored : mir_window_state_hidden;
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
