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

#include "tiling_window_manager.h"

namespace fs = std::filesystem;

// Funkcja pomocnicza do sprawdzania czy polecenie istnieje
bool command_exists(const std::string& cmd) {
    std::string check = "which " + cmd + " > /dev/null 2>&1";
    return system(check.c_str()) == 0;
}

int main(int argc, char const* argv[]) {
    miral::MirRunner runner{argc, argv};

    Config config;

    miral::ExternalClientLauncher launcher;

    TilingWindowManager* tiling_wm = nullptr;

    auto wm_policy = miral::SetWindowManagementPolicy{[&config, &tiling_wm](miral::WindowManagerTools const& tools) -> std::unique_ptr<miral::WindowManagementPolicy> {
        auto policy = std::make_unique<TilingWindowManager>(tools, config);
        tiling_wm = policy.get();
        return policy;
    }};

    // Obsługa klawiatury
    miral::AppendEventFilter input_filter{[&runner, &launcher, &tiling_wm](MirEvent const* event) -> bool {
        if (mir_event_get_type(event) != mir_event_type_input) return false;

        auto const* iev = mir_event_get_input_event(event);
        if (mir_input_event_get_type(iev) != mir_input_event_type_key) return false;

        auto const* kev = mir_input_event_get_keyboard_event(iev);
        if (mir_keyboard_event_action(kev) != mir_keyboard_action_down) return false;

        auto mods = mir_keyboard_event_modifiers(kev);
        auto key = mir_keyboard_event_scan_code(kev);

        // Alt + Esc: Wyjście
        if ((mods & mir_input_event_modifier_alt) && key == KEY_ESC) {
            runner.stop();
            return true;
        }

        // Alt + Enter: Terminal
        if ((mods & mir_input_event_modifier_alt) && key == KEY_ENTER) {
            // Preferujemy terminale, które dobrze działają na Wayland
            launcher.launch(std::vector<std::string>{"sh", "-c", "kitty || alacritty || gnome-terminal || weston-terminal || xterm"});
            return true;
        }

        // Alt + Shift + R: Restart paska
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

        // Alt + 1-5: Zmiana pulpitu
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

    // Uruchamianie aplikacji startowych (Tapeta i Pasek)
    runner.add_start_callback([&launcher] {
        // --- TAPETA ---
        std::string wallpaper_path = "/usr/share/wallpapers/HackerOS-Wallpapers/Wallpaper1.png";

        // Jeśli dedykowana tapeta nie istnieje, spróbuj znaleźć cokolwiek innego, żeby nie było czarno
        if (!fs::exists(wallpaper_path)) {
            if (fs::exists("/usr/share/backgrounds/gnome/adwaita-day.jpg")) {
                wallpaper_path = "/usr/share/backgrounds/gnome/adwaita-day.jpg";
            } else {
                wallpaper_path = ""; // Brak tapety
            }
        }

        if (!wallpaper_path.empty() && command_exists("swaybg")) {
            // Używamy swaybg jako backendu do tapety, uruchamiane wewnętrznie przez hackerland
            launcher.launch(std::vector<std::string>{"swaybg", "-i", wallpaper_path, "-m", "fill"});
        }

        // --- PASEK ---
        const char* home_env = std::getenv("HOME");
        std::string home = home_env ? home_env : "/tmp";

        std::vector<std::string> bar_paths = {
            home + "/.hackeros/hackerland/hackerland-bar",
            "./hackerland-bar",
            "/usr/local/bin/hackerland-bar",
            "/usr/bin/hackerland-bar"
        };

        bool bar_launched = false;
        for (const auto& p : bar_paths) {
            if (fs::exists(p)) {
                launcher.launch(std::vector<std::string>{p});
                bar_launched = true;
                break;
            }
        }

        if (!bar_launched) {
            launcher.launch(std::vector<std::string>{"hackerland-bar"});
        }

        // --- TERMINAL DIAGNOSTYCZNY ---
        // Uruchamiamy terminal na starcie, aby użytkownik widział, że system działa
        launcher.launch(std::vector<std::string>{"sh", "-c", "kitty || alacritty || gnome-terminal || weston-terminal"});
    });

    // Zmienne środowiskowe dla lepszego wyglądu (QT/GTK theme fix)
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "hackerland", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);

    // Sprawdź czy Xwayland jest dostępny
    bool xwayland_available = command_exists("Xwayland");

    if (xwayland_available) {
        setenv("MIR_SERVER_ENABLE_X11", "1", 1);
        // Próba wymuszenia braku dekoracji dla okien X11 (może pomóc na "brzydkie ramki")
        // Ale to zależy od klienta.
        return runner.run_with({launcher, input_filter, keymap, wm_policy, miral::X11Support{}, terminator});
    } else {
        unsetenv("MIR_SERVER_ENABLE_X11");
        return runner.run_with({launcher, input_filter, keymap, wm_policy, terminator});
    }
}
