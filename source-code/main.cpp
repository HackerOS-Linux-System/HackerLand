#include <miral/runner.h>
#include <miral/external_client.h>
#include <miral/append_event_filter.h>
#include <miral/keymap.h>
#include <miral/set_window_management_policy.h>
#include <miral/x11_support.h>
#include <miral/set_terminator.h>

#include <mir/input/event.h>

#include <linux/input-event-codes.h>

#include "tiling_window_manager.h"

int main(int argc, char const* argv[]) {
    miral::MirRunner runner{argc, argv};

    Config config;  // Domyślne z struktury

    miral::ExternalClientLauncher launcher;

    auto wm_policy = miral::SetWindowManagementPolicy{[&config](miral::WindowManagerTools const& tools) -> std::unique_ptr<miral::WindowManagementPolicy> {
        return std::make_unique<TilingWindowManager>(tools, config);
    }};

    // Rozszerzony filtr wejścia dla skrótów pulpitów
    miral::AppendEventFilter input_filter{[&runner, &wm_policy](MirEvent const* event) -> bool {
        if (mir_event_get_type(event) != mir_event_type_input) return false;

        auto const* iev = mir_event_get_input_event(event);
        if (mir_input_event_get_type(iev) != mir_input_event_type_key) return false;

        auto const* kev = mir_input_event_get_keyboard_event(iev);
        if (mir_keyboard_event_action(kev) != mir_keyboard_action_down) return false;

        auto mods = mir_keyboard_event_modifiers(kev);
        auto key = mir_keyboard_event_key_code(kev);

        if ((mods & mir_input_event_modifier_alt) && key == KEY_ESC) {
            runner.stop();
            return true;
        }

        if ((mods & mir_input_event_modifier_alt) && key == KEY_ENTER) {
            launcher.launch({"sh", "-c", "kitty || alacritty || gnome-terminal || weston-terminal"});
            return true;
        }

        // Skróty dla pulpitów: Alt + 1-5
        if (mods & mir_input_event_modifier_alt) {
            int workspace_id = -1;
            if (key == KEY_1) workspace_id = 0;
            else if (key == KEY_2) workspace_id = 1;
            else if (key == KEY_3) workspace_id = 2;
            else if (key == KEY_4) workspace_id = 3;
            else if (key == KEY_5) workspace_id = 4;

            if (workspace_id != -1) {
                // Pobierz WM i przełącz
                auto wm = dynamic_cast<TilingWindowManager*>(wm_policy.operator->());
                if (wm) {
                    wm->switch_workspace(workspace_id);
                }
                return true;
            }
        }

        return false;
    }};

    miral::Keymap keymap{"us"};

    miral::SetTerminator terminator{[](int sig) {
        exit(0);
    }};

    // UI: Uruchom zewnętrzne klienty dla tła i paska (użyj swaybg i waybar)
    runner.add_start_callback([&launcher] {
        launcher.launch({"swaybg", "-i", "/usr/share/backgrounds/gnome/adwaita-day.jpg"});  // Zmień na swoją tapetę
        launcher.launch({"waybar"});
    });

    // Poprawki środowiskowe
    setenv("MIR_X11_FREEXSYNC", "1", 1);  // Dla stabilności XWayland

    return runner.run_with({launcher, input_filter, keymap, wm_policy, miral::X11Support{}, terminator});
}
