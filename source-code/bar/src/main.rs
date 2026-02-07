use anyhow::Result;
use chrono::Local;
use gtk4::glib;
use gtk4::prelude::*;
use gtk4::{Align, Application, ApplicationWindow, Box, CssProvider, Label, Orientation};
use gtk4_layer_shell::{Edge, Layer, LayerShell};
use serde::Deserialize;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;
use sysinfo::{CpuExt, CpuRefreshKind, RefreshKind, System, SystemExt}; // Dodano CpuExt

#[derive(Deserialize, Debug)]
struct Config {
    bar: Option<BarConfig>,
}

#[derive(Deserialize, Debug)]
struct BarConfig {
    position: Option<String>,
}

#[tokio::main]
async fn main() -> Result<()> {
    let app = Application::builder()
    .application_id("com.github.twojnick.hackerbar")
    .build();

    app.connect_activate(build_ui);

    app.run();

    Ok(())
}

fn load_config() -> Config {
    let mut config_path = PathBuf::new();
    if let Some(home_dir) = dirs::home_dir() {
        config_path = home_dir.join(".config/hackerland/Config.toml");
    }

    if config_path.exists() {
        if let Ok(content) = fs::read_to_string(config_path) {
            if let Ok(cfg) = toml::from_str(&content) {
                return cfg;
            }
        }
    }

    // Default config
    Config { bar: None }
}

fn build_ui(app: &Application) {
    let config = load_config();
    let position = config.bar.and_then(|b| b.position).unwrap_or_else(|| "top".to_string());

    let window = ApplicationWindow::builder()
    .application(app)
    .title("hackerbar")
    .default_width(1920)
    .default_height(42)
    .decorated(false)
    .focusable(false)
    .build();

    window.connect_close_request(|_| {
        glib::Propagation::Stop
    });

    window.init_layer_shell();
    window.set_layer(Layer::Top);

    if position == "bottom" {
        window.set_anchor(Edge::Bottom, true);
        window.set_anchor(Edge::Top, false);
    } else {
        window.set_anchor(Edge::Top, true);
        window.set_anchor(Edge::Bottom, false);
    }

    window.set_anchor(Edge::Left, true);
    window.set_anchor(Edge::Right, true);

    window.set_margin(Edge::Top, 0);
    window.set_margin(Edge::Left, 0);
    window.set_margin(Edge::Right, 0);
    window.set_margin(Edge::Bottom, 0);

    window.auto_exclusive_zone_enable();
    window.set_namespace("hackerbar");

    let container = Box::new(Orientation::Horizontal, 0);
    container.add_css_class("bar-container");

    // --- LEWA STRONA ---
    let left_box = Box::new(Orientation::Horizontal, 10);
    left_box.add_css_class("left-module");

    let logo_label = Label::new(Some("⌘ Hackerland"));
    logo_label.add_css_class("logo-text");
    left_box.append(&logo_label);

    container.append(&left_box);

    // --- SRODEK ---
    let center_box = Box::new(Orientation::Horizontal, 0);
    center_box.set_hexpand(true);
    center_box.set_halign(Align::Center);

    let ws_label = Label::new(Some("Workspaces · 1"));
    ws_label.add_css_class("ws-pill");
    center_box.append(&ws_label);

    container.append(&center_box);

    // --- PRAWA STRONA ---
    let right_box = Box::new(Orientation::Horizontal, 8);
    right_box.add_css_class("right-module");
    right_box.set_halign(Align::End);

    let cpu_label = Label::new(Some("CPU 0%"));
    cpu_label.add_css_class("stat-pill");
    cpu_label.add_css_class("cpu-pill");
    right_box.append(&cpu_label);

    let ram_label = Label::new(Some("MEM 0.0"));
    ram_label.add_css_class("stat-pill");
    ram_label.add_css_class("ram-pill");
    right_box.append(&ram_label);

    let bat_label = Label::new(Some("BAT --"));
    bat_label.add_css_class("stat-pill");
    bat_label.add_css_class("bat-pill");
    right_box.append(&bat_label);

    let time_label = Label::new(Some("00:00"));
    time_label.add_css_class("stat-pill");
    time_label.add_css_class("time-pill");
    right_box.append(&time_label);

    container.append(&right_box);
    window.set_child(Some(&container));

    let provider = CssProvider::new();
    let style_css = r#"
    * { all: unset; font-family: "Inter", "Segoe UI", "Ubuntu", sans-serif; }

    window {
    background-color: transparent;
}

.bar-container {
background-color: #1e1e2e;
color: #cdd6f4;
padding: 0px 12px;
min-height: 42px;
border-bottom: 1px solid #313244;
}

.left-module {
margin-top: 6px;
margin-bottom: 6px;
}

.logo-text {
font-weight: 800;
font-size: 15px;
color: #cba6f7; /* Mauve */
margin-right: 15px;
letter-spacing: -0.5px;
}

.ws-pill {
background-color: #313244;
color: #ffffff;
font-weight: 600;
padding: 4px 16px;
border-radius: 99px;
margin-top: 7px;
margin-bottom: 7px;
font-size: 13px;
}

.right-module {
margin-top: 6px;
margin-bottom: 6px;
}

.stat-pill {
padding: 4px 12px;
border-radius: 8px;
font-size: 12px;
font-weight: 700;
}

.cpu-pill { background-color: #45475a; color: #f38ba8; }
.ram-pill { background-color: #45475a; color: #f9e2af; }
.bat-pill { background-color: #45475a; color: #a6e3a1; }
.time-pill { background-color: #89b4fa; color: #11111b; }
"#;

// POPRAWKA: Usunieto .as_bytes(), funkcja przyjmuje &str
provider.load_from_data(style_css);

gtk4::style_context_add_provider_for_display(
    &gtk4::gdk::Display::default().expect("Could not connect to a display."),
                                             &provider,
                                             gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
);

let mut sys = System::new_with_specifics(
    RefreshKind::new()
    .with_cpu(CpuRefreshKind::everything())
    .with_memory()
);
sys.refresh_all();

let bat_cap_path = Path::new("/sys/class/power_supply/BAT0/capacity");

glib::timeout_add_local(Duration::from_secs(1), move || {
    sys.refresh_all();

    let cpu = sys.global_cpu_info().cpu_usage();
    let ram_used = sys.used_memory() as f64 / 1024.0 / 1024.0 / 1024.0;
    let now = Local::now().format("%H:%M").to_string();

    cpu_label.set_text(&format!("CPU {:.0}%", cpu));
    ram_label.set_text(&format!("RAM {:.1}G", ram_used));
    time_label.set_text(&format!("{}", now));

    if bat_cap_path.exists() {
        if let Ok(cap_str) = fs::read_to_string(bat_cap_path) {
            let cap = cap_str.trim();
            bat_label.set_text(&format!("{}%", cap));
        }
    } else {
        bat_label.set_text("AC");
    }

    let state_path = Path::new("/tmp/hackerland_state");
    if state_path.exists() {
        if let Ok(content) = fs::read_to_string(state_path) {
            for line in content.lines() {
                if line.starts_with("workspace=") {
                    if let Some(val) = line.split('=').nth(1) {
                        ws_label.set_text(&format!("Space {}", val));
                    }
                }
            }
        }
    }

    glib::ControlFlow::Continue
});

window.present();
}
