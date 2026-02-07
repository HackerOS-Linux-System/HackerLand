#include <miral/runner.h>
#include <miral/external_client.h>
#include <miral/append_event_filter.h>
#include <miral/keymap.h>
#include <miral/set_window_management_policy.h>
#include <miral/x11_support.h>
#include <miral/set_terminator.h>
#include <miral/display_configuration.h>

#include <mir_toolkit/events/event.h>
#include <mir_toolkit/events/input/input_event.h>
#include <mir_toolkit/events/input/keyboard_event.h>

#include <linux/input-event-codes.h>
#include <cstdlib>
#include <string>
#include <iostream>
#include <filesystem>
#include <vector>
#include <fstream>
#include <algorithm>

#include "tiling_window_manager.h"

namespace fs = std::filesystem;

bool command_exists(const std::string& cmd) {
    std::string check = "which " + cmd + " > /dev/null 2>&1";
    return system(check.c_str()) == 0;
}

// Prosty parser konfigu
void load_config(Config& config, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);
            std::string val = line.substr(eq_pos + 1);

            // Trim
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t\""));
            val.erase(val.find_last_not_of(" \t\"") + 1);

            if (key == "mode") config.mode = val;
            else if (key == "bar_position") config.bar_position = val;
            else if (key == "gap_size") config.gap_size = std::stoi(val);
            else if (key == "padding") config.padding = std::stoi(val);
            else if (key == "bar_height") config.bar_height = std::stoi(val);
            else if (key == "border_width") config.border_width = std::stoi(val);
            else if (key == "corner_radius") config.corner_radius = std::stoi(val);
            else if (key == "active_border_color") config.active_border_color = val;
            else if (key == "inactive_border_color") config.inactive_border_color = val;
            else if (key.find("bind_") == 0) {
                config.keybinds[key] = val;
            }
        }
    }
}

int main(int argc, char const* argv[]) {
    miral::MirRunner runner{argc, argv};

    Config config;

    // Sprawdź flagę --config
    bool config_flag = false;
    for(int i=1; i<argc; ++i) {
        if(std::string(argv[i]) == "--config") {
            config_flag = true;
            break;
        }
    }

    // Ładuj config z pliku
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : "/tmp";
    std::string config_path = home + "/.config/hackerland/Config.toml";

    if (fs::exists(config_path)) {
        load_config(config, config_path);
    }

    if (!config_flag) {
        config.mode = "tiling";
    }

    miral::ExternalClientLauncher launcher;

    TilingWindowManager* tiling_wm = nullptr;

    auto wm_policy = miral::SetWindowManagementPolicy{[&config, &tiling_wm](miral::WindowManagerTools const& tools) -> std::unique_ptr<miral::WindowManagementPolicy> {
        auto policy = std::make_unique<TilingWindowManager>(tools, config);
        tiling_wm = policy.get();
        return policy;
    }};

    // Obsługa klawiatury
    miral::AppendEventFilter input_filter{[&runner, &launcher, &tiling_wm, &config](MirEvent const* event) -> bool {
        if (mir_event_get_type(event) != mir_event_type_input) return false;

        auto const* iev = mir_event_get_input_event(event);
        if (mir_input_event_get_type(iev) != mir_input_event_type_key) return false;

        auto const* kev = mir_input_event_get_keyboard_event(iev);
        if (mir_keyboard_event_action(kev) != mir_keyboard_action_down) return false;

        auto mods = mir_keyboard_event_modifiers(kev);
        auto key = mir_keyboard_event_scan_code(kev);

        // Systemowe skróty
        if ((mods & mir_input_event_modifier_alt) && key == KEY_ESC) {
            runner.stop();
            return true;
        }

        // TERMINAL LAUNCHER
        if ((mods & mir_input_event_modifier_alt) && key == KEY_ENTER) {
            if (command_exists("alacritty")) {
                launcher.launch(std::vector<std::string>{"alacritty"});
            } else if (command_exists("konsole")) {
                launcher.launch(std::vector<std::string>{"konsole"});
            } else if (command_exists("kitty")) {
                launcher.launch(std::vector<std::string>{"kitty"});
            } else if (command_exists("gnome-terminal")) {
                launcher.launch(std::vector<std::string>{"gnome-terminal"});
            } else {
                launcher.launch(std::vector<std::string>{"xterm"});
            }
            return true;
        }

        // Restart paska
        if ((mods & mir_input_event_modifier_alt) && (mods & mir_input_event_modifier_shift) && key == KEY_R) {
            const char* home_env = std::getenv("HOME");
            std::string home = home_env ? home_env : "/tmp";
            std::string bar_path = home + "/.hackeros/hackerland/hackerland-bar";
            if (fs::exists(bar_path)) {
                launcher.launch(std::vector<std::string>{bar_path});
            } else {
                launcher.launch(std::vector<std::string>{"hackerland-bar"});
            }
            return true;
        }

        // Workspace switching
        if (mods & mir_input_event_modifier_alt) {
            int workspace_id = -1;
            if (key == KEY_1) workspace_id = 0;
            else if (key == KEY_2) workspace_id = 1;
            else if (key == KEY_3) workspace_id = 2;
            else if (key == KEY_4) workspace_id = 3;
            else if (key == KEY_5) workspace_id = 4;

            if (workspace_id != -1 && tiling_wm) {
                tiling_wm->switch_workspace(workspace_id);
                return true;
            }
        }

        return false;
    }};

    miral::Keymap keymap{"us"};

    miral::SetTerminator terminator{[](int sig) {
        exit(0);
    }};

    runner.add_start_callback([&launcher, &config] {
        const char* home_env = std::getenv("HOME");
        std::string home = home_env ? home_env : "/tmp";

        // --- HACKERLAND-BG (Rust + Smithay) ---
        std::string bg_path = home + "/.hackeros/hackerland/hackerland-bg";
        if (!fs::exists(bg_path)) bg_path = "./hackerland-bg";

        if (fs::exists(bg_path)) {
            launcher.launch(std::vector<std::string>{bg_path});
        }

        // --- HACKERLAND-BAR ---
        if (config.enable_bar) {
            std::string bar_path = home + "/.hackeros/hackerland/hackerland-bar";
            std::vector<std::string> bar_paths = {
                bar_path,
                "./hackerland-bar",
                "/usr/bin/hackerland-bar"
            };

            for (const auto& p : bar_paths) {
                if (fs::exists(p)) {
                    launcher.launch(std::vector<std::string>{p});
                    break;
                }
            }
        }
    });

    setenv("GDK_BACKEND", "wayland", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "hackerland", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);

    bool xwayland_available = command_exists("Xwayland");

    if (xwayland_available) {
        setenv("MIR_SERVER_ENABLE_X11", "1", 1);
        return runner.run_with({launcher, input_filter, keymap, wm_policy, miral::X11Support{}, terminator});
    } else {
        unsetenv("MIR_SERVER_ENABLE_X11");
        return runner.run_with({launcher, input_filter, keymap, wm_policy, terminator});
    }
}
