use anyhow::Result;
use chrono::Local;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::{Align, Application, ApplicationWindow, Box, CssProvider, Label, Orientation};
use gtk4_layer_shell::{Edge, Layer, LayerShell};
use std::fs;
use std::path::Path;
use std::time::Duration;
use sysinfo::{CpuRefreshKind, MemoryRefreshKind, RefreshKind, System};

#[tokio::main]
async fn main() -> Result<()> {
    let app = Application::builder()
    .application_id("com.github.twojnick.hackerbar")
    .build();

    app.connect_activate(build_ui);

    app.run();

    Ok(())
}

fn build_ui(app: &Application) {
    let window = ApplicationWindow::builder()
    .application(app)
    .title("hackerbar")
    .default_width(1920)
    .default_height(30)
    .decorated(false) // WAŻNE: Wyłącza dekoracje systemowe (pasek tytułu)
    .focusable(false) // WAŻNE: Pasek nie powinien przyjmować focusa (klawiatury/klikania)
    .build();

    // WAŻNE: Zablokowanie możliwości zamknięcia okna (np. przez Alt+F4)
    window.connect_close_request(|_| {
        glib::Propagation::Stop
    });

    // Konfiguracja Layer Shell - to pozycjonuje pasek na górze
    window.init_layer_shell();
    window.set_layer(Layer::Top);

    // Kotwice - przyklej do góry, lewej i prawej
    window.set_anchor(Edge::Top, true);
    window.set_anchor(Edge::Left, true);
    window.set_anchor(Edge::Right, true);

    // Marginesy - upewnij się, że są zerowe
    window.set_margin(Edge::Top, 0);
    window.set_margin(Edge::Left, 0);
    window.set_margin(Edge::Right, 0);

    // Ekskluzywna strefa (odsuwa inne okna)
    window.auto_exclusive_zone_enable();

    // Namespace dla kompozytora
    window.set_namespace(Some("hackerbar"));

    let container = Box::new(Orientation::Horizontal, 0);
    container.add_css_class("bar-container");

    // LEFT: Logo / App Name
    let logo_box = Box::new(Orientation::Horizontal, 0);
    logo_box.add_css_class("logo-box");
    let logo_label = Label::new(Some("HACKERLAND"));
    logo_label.add_css_class("logo-text");
    logo_box.append(&logo_label);
    container.append(&logo_box);

    // SPACER
    let spacer = Box::new(Orientation::Horizontal, 0);
    spacer.set_hexpand(true);
    container.append(&spacer);

    // RIGHT: Stats (CPU, RAM, BAT, Time)
    let right_box = Box::new(Orientation::Horizontal, 15);
    right_box.add_css_class("right-box");
    right_box.set_halign(Align::End);

    // Workspace Indicator
    let ws_label = Label::new(Some("WS: 1"));
    ws_label.add_css_class("stat-item");
    ws_label.add_css_class("ws-item");
    right_box.append(&ws_label);

    // CPU
    let cpu_label = Label::new(Some("CPU 0%"));
    cpu_label.add_css_class("stat-item");
    right_box.append(&cpu_label);

    // RAM
    let ram_label = Label::new(Some("MEM 0.0"));
    ram_label.add_css_class("stat-item");
    right_box.append(&ram_label);

    // BATTERY (Expanded)
    let bat_label = Label::new(Some("BAT --"));
    bat_label.add_css_class("stat-item");
    bat_label.add_css_class("bat-item");
    right_box.append(&bat_label);

    // TIME
    let time_label = Label::new(Some("00:00"));
    time_label.add_css_class("stat-item");
    time_label.add_css_class("time-item");
    right_box.append(&time_label);

    container.append(&right_box);
    window.set_child(Some(&container));

    // CSS STYLING
    let provider = CssProvider::new();
    provider.load_from_string(
        r#"
        * { all: unset; }
        window { background-color: #000000; color: #ffffff; font-family: Monospace; font-size: 14px; }
        .bar-container { padding: 4px; min-height: 30px; }
        .logo-box { padding: 0px 10px; font-weight: bold; color: #00ff00; }
        .logo-text { font-weight: bold; }
        .right-box { padding-right: 10px; }
        .stat-item { margin-left: 15px; color: #cccccc; }
        .ws-item { font-weight: bold; color: #ffffff; }
        .bat-item { color: #ffffff; }
        .time-item { color: #ffffff; }
        "#,
    );

    gtk4::style_context_add_provider_for_display(
        &gtk4::gdk::Display::default().expect("Could not connect to a display."),
                                                 &provider,
                                                 gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );

    let mut sys = System::new_with_specifics(
        RefreshKind::new()
        .with_cpu(CpuRefreshKind::everything())
        .with_memory(MemoryRefreshKind::everything())
    );
    sys.refresh_all();

    // Paths to battery (Linux standard)
    let bat_cap_path = Path::new("/sys/class/power_supply/BAT0/capacity");
    let bat_status_path = Path::new("/sys/class/power_supply/BAT0/status");

    glib::timeout_add_local(Duration::from_secs(1), move || {
        sys.refresh_all();

        // Update Stats
        let cpu = sys.global_cpu_info().cpu_usage();
        let ram_used = sys.used_memory() as f64 / 1024.0 / 1024.0 / 1024.0;
        let now = Local::now().format("%H:%M").to_string();

        cpu_label.set_text(&format!(" {:.0}%", cpu));
        ram_label.set_text(&format!(" {:.2}G", ram_used));
        time_label.set_text(&format!(" {}", now));

        // Battery Logic
        if bat_cap_path.exists() {
            if let Ok(cap_str) = fs::read_to_string(bat_cap_path) {
                let cap = cap_str.trim();
                let icon = if let Ok(val) = cap.parse::<i32>() {
                    if val > 80 { "" } else if val > 50 { "" } else if val > 20 { "" } else { "" }
                } else { "" };
                bat_label.set_text(&format!("{} {}%", icon, cap));
            }
        } else {
            // Jeśli brak baterii (desktop), ukryj lub pokaż AC
            bat_label.set_text(" AC");
        }

        // Update Workspace from File
        let state_path = Path::new("/tmp/hackerland_state");
        if state_path.exists() {
            if let Ok(content) = fs::read_to_string(state_path) {
                for line in content.lines() {
                    if line.starts_with("workspace=") {
                        if let Some(val) = line.split('=').nth(1) {
                            ws_label.set_text(&format!("  {}", val));
                        }
                    }
                }
            }
        }

        glib::ControlFlow::Continue
    });

    window.present();
}
