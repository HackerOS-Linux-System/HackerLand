#include <miral/runner.h>
#include <miral/external_client.h>
#include <miral/append_event_filter.h>
#include <miral/keymap.h>
#include <miral/set_window_management_policy.h>
#include <miral/x11_support.h>
#include <miral/set_terminator.h>
#include <mir_toolkit/events/event.h>
#include <linux/input-event-codes.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include "tiling_window_manager.h"

namespace fs = std::filesystem;

// --- Helper: Command Execution ---
bool command_exists(const std::string& cmd) {
    std::string check = "which " + cmd + " > /dev/null 2>&1";
    return system(check.c_str()) == 0;
}

// --- Helper: Config Loader ---
void load_config(Config& config, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Simple trim
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key); trim(val);

        if (key == "gap_size") config.gap_size = std::stoi(val);
        else if (key == "padding") config.padding = std::stoi(val);
        else if (key == "bar_height") config.bar_height = std::stoi(val);
        else if (key == "border_width") config.border_width = std::stoi(val);
        else if (key == "spring_tension") config.spring_tension = std::stod(val);
        else if (key == "spring_friction") config.spring_friction = std::stod(val);
        else if (key == "bar_position") config.bar_position = val;
        else if (key.rfind("bindsym ", 0) == 0) { // e.g., bindsym Alt+Enter
            std::string action = key.substr(8);
            trim(action);
            config.keybinds[action] = val;
        }
    }
}

// --- Helper: Key String Mapper ---
int get_keycode_from_name(const std::string& name) {
    static const std::map<std::string, int> key_map = {
        {"Enter", KEY_ENTER}, {"Return", KEY_ENTER}, {"Space", KEY_SPACE},
        {"Esc", KEY_ESC}, {"Tab", KEY_TAB}, {"Backspace", KEY_BACKSPACE},
        {"Q", KEY_Q}, {"W", KEY_W}, {"E", KEY_E}, {"R", KEY_R}, {"T", KEY_T},
        {"Y", KEY_Y}, {"U", KEY_U}, {"I", KEY_I}, {"O", KEY_O}, {"P", KEY_P},
        {"A", KEY_A}, {"S", KEY_S}, {"D", KEY_D}, {"F", KEY_F}, {"G", KEY_G},
        {"H", KEY_H}, {"J", KEY_J}, {"K", KEY_K}, {"L", KEY_L},
        {"Z", KEY_Z}, {"X", KEY_X}, {"C", KEY_C}, {"V", KEY_V}, {"B", KEY_B},
        {"N", KEY_N}, {"M", KEY_M},
        {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
        {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0}
    };
    auto it = key_map.find(name);
    return (it != key_map.end()) ? it->second : 0;
}

int main(int argc, char const* argv[]) {
    miral::MirRunner runner{argc, argv};
    miral::ExternalClientLauncher launcher;

    Config config;
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : "/tmp";
    std::string config_path = home + "/.config/hackerland/Config.toml";

    if (fs::exists(config_path)) load_config(config, config_path);

    TilingWindowManager* wm_ptr = nullptr;

    auto wm_policy = miral::SetWindowManagementPolicy{[&](miral::WindowManagerTools const& tools) {
        auto wm = std::make_unique<TilingWindowManager>(tools, config);
        wm_ptr = wm.get();
        return wm;
    }};

    auto input_filter = miral::AppendEventFilter{[&](MirEvent const* event) -> bool {
        if (mir_event_get_type(event) != mir_event_type_input) return false;
        auto iev = mir_event_get_input_event(event);
        if (mir_input_event_get_type(iev) != mir_input_event_type_key) return false;
        auto kev = mir_input_event_get_keyboard_event(iev);
        if (mir_keyboard_event_action(kev) != mir_keyboard_action_down) return false;

        auto key = mir_keyboard_event_scan_code(kev);
        auto mods = mir_keyboard_event_modifiers(kev);

        bool alt = (mods & mir_input_event_modifier_alt);
        bool shift = (mods & mir_input_event_modifier_shift);
        bool ctrl = (mods & mir_input_event_modifier_ctrl);
        bool super = (mods & mir_input_event_modifier_meta);

        // Dynamic Keybinds
        for (const auto& [bind, cmd_str] : config.keybinds) {
            // Check modifiers match
            bool req_alt = bind.find("Alt") != std::string::npos;
            bool req_shift = bind.find("Shift") != std::string::npos;
            bool req_ctrl = bind.find("Ctrl") != std::string::npos;
            bool req_super = bind.find("Super") != std::string::npos;

            if (alt != req_alt || shift != req_shift || ctrl != req_ctrl || super != req_super) continue;

            // Extract key name (last token after '+')
            size_t last_plus = bind.rfind('+');
            std::string key_name = (last_plus == std::string::npos) ? bind : bind.substr(last_plus + 1);

            if (key == get_keycode_from_name(key_name)) {
                // Split command string into args
                std::istringstream iss(cmd_str);
                std::vector<std::string> args;
                std::string arg;
                while (iss >> arg) args.push_back(arg);
                if (!args.empty()) launcher.launch(args);
                return true;
            }
        }

        // Hardcoded Fallbacks (for safety and WM control)
        if (alt && key == KEY_ENTER) {
            launcher.launch(std::vector<std::string>{"alacritty"});
            return true;
        }
        if (alt && key == KEY_D) {
            launcher.launch(std::vector<std::string>{"rofi", "-show", "drun"});
            return true;
        }
        if (alt && shift && key == KEY_Q) {
            runner.stop();
            return true;
        }

        if (wm_ptr) {
            if (alt && key == KEY_SPACE) { wm_ptr->cycle_layout(); return true; }
            if (alt && key == KEY_H) { wm_ptr->resize_master(-0.05); return true; }
            if (alt && key == KEY_L) { wm_ptr->resize_master(0.05); return true; }

            if (alt && key == KEY_1) { wm_ptr->switch_workspace(0); return true; }
            if (alt && key == KEY_2) { wm_ptr->switch_workspace(1); return true; }
            if (alt && key == KEY_3) { wm_ptr->switch_workspace(2); return true; }
            if (alt && key == KEY_4) { wm_ptr->switch_workspace(3); return true; }
            if (alt && key == KEY_5) { wm_ptr->switch_workspace(4); return true; }
        }

        return false;
    }};

    runner.add_start_callback([&] {
        // Updated Path: Look for bar in .hackeros/hackerland
        std::string bar_path = home + "/.hackeros/hackerland/hackerland-bar";
        if (fs::exists(bar_path)) {
            launcher.launch(std::vector<std::string>{bar_path});
        }

        // Auto-start background (optional check)
        std::string bg = home + "/.config/hackerland/bg.png";
        if (fs::exists(bg)) launcher.launch(std::vector<std::string>{"swaybg", "-i", bg, "-m", "fill"});
    });

    return runner.run_with({
        wm_policy,
        input_filter,
        launcher,
        miral::Keymap{},
        miral::X11Support{},
        miral::SetTerminator{{0}}
    });
}
