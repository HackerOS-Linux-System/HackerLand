use anyhow::Result;
use chrono::Local;
use gtk4::glib; // Importujemy glib bezpośrednio z gtk4
use gtk4::prelude::*;
use gtk4::{Align, Application, ApplicationWindow, Box, CssProvider, Label, Orientation};
use gtk4_layer_shell::{Edge, Layer, LayerShell};
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
        .default_height(32)
        .build();

    // Inicjalizacja layer-shell
    window.init_layer_shell();
    window.set_layer(Layer::Top);
    
    // W Rust bindings dla layer-shell ustawiamy kotwice (anchors) pojedynczo
    window.set_anchor(Edge::Top, true);
    window.set_anchor(Edge::Left, true);
    window.set_anchor(Edge::Right, true);
    
    window.set_exclusive_zone(32);
    // FIX: set_namespace wymaga Option<&str>
    window.set_namespace(Some("hackerbar"));

    let container = Box::new(Orientation::Horizontal, 12);
    container.set_hexpand(true);
    container.set_vexpand(true);
    container.set_margin_start(16);
    container.set_margin_end(16);
    container.add_css_class("hackerbar");

    let left_label = Label::new(Some("HACKERMODE | "));
    left_label.add_css_class("left-label");
    container.append(&left_label);

    let info_label = Label::new(None);
    info_label.set_hexpand(true);
    info_label.set_halign(Align::End);
    info_label.add_css_class("info-label");
    container.append(&info_label);

    window.set_child(Some(&container));

    let provider = CssProvider::new();
    // FIX: load_from_data jest deprecated, używamy load_from_string
    provider.load_from_string(
        r#"
        window {
            background-color: rgba(10, 10, 20, 0.85);
            color: #00d4ff;
            font-family: "JetBrains Mono", "Fira Code", monospace;
            font-size: 13px;
            border-bottom: 1px solid #00d4ff44;
        }
        .hackerbar {
            background: transparent;
        }
        .left-label {
            color: #ff0055;
            font-weight: bold;
        }
        .info-label {
            color: #00ff9d;
        }
        "#,
    );
    
    gtk4::style_context_add_provider_for_display(
        &gtk4::gdk::Display::default().expect("Could not connect to a display."),
        &provider,
        gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );

    // Inicjalizacja Systemu MUSI być przed pętlą, aby poprawnie liczyć zużycie CPU (delta czasu)
    let mut sys = System::new_with_specifics(
        RefreshKind::new()
            .with_cpu(CpuRefreshKind::everything())
            .with_memory(MemoryRefreshKind::everything())
    );

    // Pierwsze odświeżenie, aby zebrać dane bazowe
    sys.refresh_all();

    glib::timeout_add_local(Duration::from_secs(1), move || {
        sys.refresh_all();

        let now = Local::now().format("%H:%M:%S").to_string();
        
        // W zależności od wersji sysinfo, globalne użycie CPU pobieramy tak:
        let cpu = sys.global_cpu_info().cpu_usage(); 
        
        let ram_used = sys.used_memory() / 1024 / 1024;
        let ram_total = sys.total_memory() / 1024 / 1024;

        let text = format!("CPU: {:.1}%  |  RAM: {}/{} MiB  |  {}", cpu, ram_used, ram_total, now);
        info_label.set_text(&text);

        glib::ControlFlow::Continue
    });

    window.present();
}
