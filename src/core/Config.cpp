#include "Config.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QColor>
#include <QTextStream>
#include <QRegularExpression>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal TOML parser — supports the subset used by HackerLand config:
//   [section]
//   key = "string"
//   key = 123
//   key = 1.5
//   key = true
//   key = "#rrggbb"
//   # comment
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    struct TomlValue {
        QString raw;   // trimmed raw value (without surrounding quotes)
        bool    isStr; // was the value a quoted string?
    };

    using TomlSection = QMap<QString, TomlValue>;
    using TomlDoc     = QMap<QString, TomlSection>; // section -> key -> value

    static TomlDoc parseTOMLDoc(const QString& text) {
        TomlDoc doc;
        QString section = "__root__";
        const QStringList lines = text.split('\n');

        for (QString line : lines) {
            // Strip inline comment
            const int commentPos = line.indexOf('#');
            if (commentPos >= 0) line = line.left(commentPos);
            line = line.trimmed();
            if (line.isEmpty()) continue;

            // Section header [foo] or [foo.bar]
            if (line.startsWith('[') && line.endsWith(']')) {
                section = line.mid(1, line.size() - 2).trimmed();
                continue;
            }

            // key = value
            const int eq = line.indexOf('=');
            if (eq < 0) continue;

            const QString key = line.left(eq).trimmed();
            QString val       = line.mid(eq + 1).trimmed();

            TomlValue tv;
            if ((val.startsWith('"') && val.endsWith('"')) ||
                (val.startsWith('\'') && val.endsWith('\''))) {
                tv.raw   = val.mid(1, val.size() - 2);
            tv.isStr = true;
                } else {
                    tv.raw   = val;
                    tv.isStr = false;
                }
                doc[section][key] = tv;
        }
        return doc;
    }

    static QString tStr(const TomlSection& s, const char* k, const QString& def) {
        auto it = s.constFind(k);
        return (it != s.constEnd()) ? it->raw : def;
    }

    static int tInt(const TomlSection& s, const char* k, int def) {
        auto it = s.constFind(k);
        if (it == s.constEnd()) return def;
        bool ok; int v = it->raw.toInt(&ok);
        return ok ? v : def;
    }

    static float tFloat(const TomlSection& s, const char* k, float def) {
        auto it = s.constFind(k);
        if (it == s.constEnd()) return def;
        bool ok; float v = it->raw.toFloat(&ok);
        return ok ? v : def;
    }

    static bool tBool(const TomlSection& s, const char* k, bool def) {
        auto it = s.constFind(k);
        if (it == s.constEnd()) return def;
        const QString v = it->raw.toLower();
        if (v == "true"  || v == "1" || v == "yes") return true;
        if (v == "false" || v == "0" || v == "no")  return false;
        return def;
    }

    static QColor tColor(const TomlSection& s, const char* k, const QColor& def) {
        auto it = s.constFind(k);
        if (it == s.constEnd()) return def;
        QColor c(it->raw);
        return c.isValid() ? c : def;
    }

    static QString colorStr(const QColor& c) {
        return c.name(QColor::HexArgb); // #aarrggbb
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

Config& Config::instance() {
    static Config inst;
    return inst;
}

Config::Config() {
    setDefaults();
}

// ─────────────────────────────────────────────────────────────────────────────
// Default config path
// ─────────────────────────────────────────────────────────────────────────────

QString Config::defaultConfigPath() {
    return QDir::homePath() + "/.config/hackeros/hackerland/config.toml";
}

// ─────────────────────────────────────────────────────────────────────────────
// Load
// ─────────────────────────────────────────────────────────────────────────────

bool Config::load(const QString& path) {
    const QString target = path.isEmpty() ? defaultConfigPath() : path;
    m_loadedPath = target;

    QFile f(target);
    if (!f.exists()) {
        qInfo() << "[Config] file not found:" << target
        << "— generating with defaults";
        setDefaults();
        loadDefaultKeybinds();
        save(target);  // auto-generate
        return true;
    }

    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[Config] cannot open:" << target;
        setDefaults();
        loadDefaultKeybinds();
        return false;
    }

    const QString text = QTextStream(&f).readAll();
    f.close();

    setDefaults();
    loadDefaultKeybinds();
    const bool ok = parseTOML(text);
    if (ok) qInfo() << "[Config] loaded:" << target;
    return ok;
}

bool Config::reload() {
    return load(m_loadedPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// TOML parser  (populates structs from TomlDoc)
// ─────────────────────────────────────────────────────────────────────────────

bool Config::parseTOML(const QString& text) {
    const TomlDoc doc = parseTOMLDoc(text);

    // ── [compositor] ─────────────────────────────────────────────────────
    if (doc.contains("compositor")) {
        const auto& s = doc["compositor"];
        const QString m = tStr(s, "mode", "tiling").toLower();
        if      (m == "cage")       mode = CompositorMode::Cage;
        else if (m == "gamescope")  mode = CompositorMode::Gamescope;
        else                        mode = CompositorMode::Tiling;

        m_workspaceCount = tInt(s, "workspaces", m_workspaceCount);
    }

    // ── [cage] ────────────────────────────────────────────────────────────
    if (doc.contains("cage")) {
        const auto& s   = doc["cage"];
        cage.exec        = tStr  (s, "exec",         cage.exec);
        cage.exitOnClose = tBool (s, "exit_on_close", cage.exitOnClose);
        cage.allowVt     = tBool (s, "allow_vt",      cage.allowVt);
    }

    // ── [gamescope] ───────────────────────────────────────────────────────
    if (doc.contains("gamescope")) {
        const auto& s        = doc["gamescope"];
        gamescope.exec        = tStr  (s, "exec",          gamescope.exec);
        gamescope.renderW     = tInt  (s, "render_width",   gamescope.renderW);
        gamescope.renderH     = tInt  (s, "render_height",  gamescope.renderH);
        gamescope.outputW     = tInt  (s, "output_width",   gamescope.outputW);
        gamescope.outputH     = tInt  (s, "output_height",  gamescope.outputH);
        gamescope.fpsLimit    = tInt  (s, "fps_limit",      gamescope.fpsLimit);
        gamescope.integerScale= tBool (s, "integer_scale",  gamescope.integerScale);
        gamescope.fullscreen  = tBool (s, "fullscreen",     gamescope.fullscreen);
        gamescope.borderless  = tBool (s, "borderless",     gamescope.borderless);
        gamescope.filter      = tStr  (s, "filter",         gamescope.filter);
        gamescope.exitOnClose = tBool (s, "exit_on_close",  gamescope.exitOnClose);
    }

    // ── [theme] ───────────────────────────────────────────────────────────
    if (doc.contains("theme")) {
        const auto& s            = doc["theme"];
        theme.accentColor         = tColor(s, "accent",            theme.accentColor);
        theme.accentSecondary     = tColor(s, "accent_secondary",   theme.accentSecondary);
        theme.accentTertiary      = tColor(s, "accent_tertiary",    theme.accentTertiary);
        theme.glassBackground     = tColor(s, "glass_background",   theme.glassBackground);
        theme.glassBorder         = tColor(s, "glass_border",       theme.glassBorder);
        theme.glassBorderActive   = tColor(s, "glass_border_active",theme.glassBorderActive);
        theme.textPrimary         = tColor(s, "text_primary",       theme.textPrimary);
        theme.textSecondary       = tColor(s, "text_secondary",     theme.textSecondary);
        theme.textMuted           = tColor(s, "text_muted",         theme.textMuted);
        theme.barBackground       = tColor(s, "bar_background",     theme.barBackground);
        theme.barBorder           = tColor(s, "bar_border",         theme.barBorder);
        theme.barHeight           = tInt  (s, "bar_height",         theme.barHeight);
        theme.barBlur             = tBool (s, "bar_blur",           theme.barBlur);
        theme.borderWidth         = tInt  (s, "border_width",       theme.borderWidth);
        theme.borderRadius        = tInt  (s, "border_radius",      theme.borderRadius);
        theme.gapInner            = tInt  (s, "gap_inner",          theme.gapInner);
        theme.gapOuter            = tInt  (s, "gap_outer",          theme.gapOuter);
        theme.inactiveOpacity     = tFloat(s, "inactive_opacity",   theme.inactiveOpacity);
        theme.activeOpacity       = tFloat(s, "active_opacity",     theme.activeOpacity);
        theme.blurRadius          = tFloat(s, "blur_radius",        theme.blurRadius);
        theme.fontFamily          = tStr  (s, "font_family",        theme.fontFamily);
        theme.fontSizeBar         = tInt  (s, "font_size_bar",      theme.fontSizeBar);
        theme.fontSizeUI          = tInt  (s, "font_size_ui",       theme.fontSizeUI);
        theme.wallpaperPath       = tStr  (s, "wallpaper",          theme.wallpaperPath);
        theme.wallpaperMode       = tStr  (s, "wallpaper_mode",     theme.wallpaperMode);
    }

    // ── [animations] ──────────────────────────────────────────────────────
    if (doc.contains("animations")) {
        const auto& s         = doc["animations"];
        anim.enabled           = tBool (s, "enabled",            anim.enabled);
        anim.windowOpenMs      = tInt  (s, "window_open_ms",     anim.windowOpenMs);
        anim.windowCloseMs     = tInt  (s, "window_close_ms",    anim.windowCloseMs);
        anim.workspaceSwitchMs = tInt  (s, "workspace_switch_ms",anim.workspaceSwitchMs);
        anim.tileRearrangeMs   = tInt  (s, "tile_rearrange_ms",  anim.tileRearrangeMs);
        anim.scaleFactor       = tFloat(s, "scale_factor",       anim.scaleFactor);
    }

    // ── [tiling] ──────────────────────────────────────────────────────────
    if (doc.contains("tiling")) {
        const auto& s         = doc["tiling"];
        tiling.layout          = tStr  (s, "layout",              tiling.layout);
        tiling.masterRatio     = tFloat(s, "master_ratio",        tiling.masterRatio);
        tiling.maxColumns      = tInt  (s, "max_columns",         tiling.maxColumns);
        tiling.smartGaps       = tBool (s, "smart_gaps",          tiling.smartGaps);
        tiling.smartBorders    = tBool (s, "smart_borders",       tiling.smartBorders);
        tiling.centerSingle    = tBool (s, "center_single",       tiling.centerSingle);
        tiling.centerSingleScale= tFloat(s,"center_single_scale", tiling.centerSingleScale);
    }

    // ── [keybinds] ────────────────────────────────────────────────────────
    if (doc.contains("keybinds")) {
        const auto& s = doc["keybinds"];
        keys.modifier  = tStr(s, "modifier", keys.modifier);
        for (auto it = s.constBegin(); it != s.constEnd(); ++it) {
            if (it.key() == "modifier") continue;
            keys.bindings[it.key()] = it->raw;
        }
    }

    emit configReloaded();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialize to TOML
// ─────────────────────────────────────────────────────────────────────────────

QString Config::serializeTOML() const {
    QString t;
    QTextStream s(&t);

    s << "# HackerLand WM — config.toml\n"
    << "# Generated automatically. Edit freely.\n"
    << "# Location: ~/.config/hackeros/hackerland/config.toml\n\n";

    // ── [compositor] ──────────────────────────────────────────────────────
    s << "[compositor]\n";
    s << "# mode = \"tiling\" | \"cage\" | \"gamescope\"\n";
    switch (mode) {
        case CompositorMode::Cage:      s << "mode = \"cage\"\n";      break;
        case CompositorMode::Gamescope: s << "mode = \"gamescope\"\n"; break;
        default:                        s << "mode = \"tiling\"\n";    break;
    }
    s << "workspaces = " << m_workspaceCount << "\n\n";

    // ── [cage] ────────────────────────────────────────────────────────────
    s << "[cage]\n"
    << "# Command to run in kiosk/cage mode (used when mode = \"cage\")\n"
    << "exec         = \"" << cage.exec << "\"\n"
    << "exit_on_close = " << (cage.exitOnClose ? "true" : "false") << "\n"
    << "allow_vt      = " << (cage.allowVt     ? "true" : "false") << "\n\n";

    // ── [gamescope] ───────────────────────────────────────────────────────
    s << "[gamescope]\n"
    << "# Command to run inside the micro-compositor (mode = \"gamescope\")\n"
    << "exec          = \"" << gamescope.exec << "\"\n"
    << "render_width  = " << gamescope.renderW << "\n"
    << "render_height = " << gamescope.renderH << "\n"
    << "output_width  = " << gamescope.outputW << "\n"
    << "output_height = " << gamescope.outputH << "\n"
    << "fps_limit     = " << gamescope.fpsLimit << "\n"
    << "integer_scale = " << (gamescope.integerScale ? "true" : "false") << "\n"
    << "fullscreen    = " << (gamescope.fullscreen   ? "true" : "false") << "\n"
    << "borderless    = " << (gamescope.borderless   ? "true" : "false") << "\n"
    << "filter        = \"" << gamescope.filter << "\"\n"
    << "exit_on_close = " << (gamescope.exitOnClose  ? "true" : "false") << "\n\n";

    // ── [theme] ───────────────────────────────────────────────────────────
    s << "[theme]\n"
    << "accent              = \"" << colorStr(theme.accentColor)       << "\"\n"
    << "accent_secondary    = \"" << colorStr(theme.accentSecondary)   << "\"\n"
    << "accent_tertiary     = \"" << colorStr(theme.accentTertiary)    << "\"\n"
    << "glass_background    = \"" << colorStr(theme.glassBackground)   << "\"\n"
    << "glass_border        = \"" << colorStr(theme.glassBorder)       << "\"\n"
    << "glass_border_active = \"" << colorStr(theme.glassBorderActive) << "\"\n"
    << "text_primary        = \"" << colorStr(theme.textPrimary)       << "\"\n"
    << "text_secondary      = \"" << colorStr(theme.textSecondary)     << "\"\n"
    << "text_muted          = \"" << colorStr(theme.textMuted)         << "\"\n"
    << "bar_background      = \"" << colorStr(theme.barBackground)     << "\"\n"
    << "bar_border          = \"" << colorStr(theme.barBorder)         << "\"\n"
    << "bar_height          = "   << theme.barHeight                   << "\n"
    << "bar_blur            = "   << (theme.barBlur ? "true":"false")  << "\n"
    << "border_width        = "   << theme.borderWidth                 << "\n"
    << "border_radius       = "   << theme.borderRadius                << "\n"
    << "gap_inner           = "   << theme.gapInner                    << "\n"
    << "gap_outer           = "   << theme.gapOuter                    << "\n"
    << "inactive_opacity    = "   << theme.inactiveOpacity             << "\n"
    << "active_opacity      = "   << theme.activeOpacity               << "\n"
    << "blur_radius         = "   << theme.blurRadius                  << "\n"
    << "font_family         = \"" << theme.fontFamily                  << "\"\n"
    << "font_size_bar       = "   << theme.fontSizeBar                 << "\n"
    << "font_size_ui        = "   << theme.fontSizeUI                  << "\n"
    << "wallpaper           = \"" << theme.wallpaperPath               << "\"\n"
    << "wallpaper_mode      = \"" << theme.wallpaperMode               << "\"\n\n";

    // ── [animations] ──────────────────────────────────────────────────────
    s << "[animations]\n"
    << "enabled             = " << (anim.enabled ? "true" : "false")  << "\n"
    << "window_open_ms      = " << anim.windowOpenMs                  << "\n"
    << "window_close_ms     = " << anim.windowCloseMs                 << "\n"
    << "workspace_switch_ms = " << anim.workspaceSwitchMs             << "\n"
    << "tile_rearrange_ms   = " << anim.tileRearrangeMs               << "\n"
    << "scale_factor        = " << anim.scaleFactor                   << "\n\n";

    // ── [tiling] ──────────────────────────────────────────────────────────
    s << "[tiling]\n"
    << "# layout: spiral | tall | wide | grid | dwindle | monocle | bsp | centered\n"
    << "layout              = \"" << tiling.layout           << "\"\n"
    << "master_ratio        = "   << tiling.masterRatio      << "\n"
    << "max_columns         = "   << tiling.maxColumns       << "\n"
    << "smart_gaps          = "   << (tiling.smartGaps     ? "true":"false") << "\n"
    << "smart_borders       = "   << (tiling.smartBorders  ? "true":"false") << "\n"
    << "center_single       = "   << (tiling.centerSingle  ? "true":"false") << "\n"
    << "center_single_scale = "   << tiling.centerSingleScale << "\n\n";

    // ── [keybinds] ────────────────────────────────────────────────────────
    s << "[keybinds]\n"
    << "# modifier: Super | Alt | Ctrl\n"
    << "modifier = \"" << keys.modifier << "\"\n";
    for (auto it = keys.bindings.constBegin(); it != keys.bindings.constEnd(); ++it) {
        s << it.key() << " = \"" << it.value() << "\"\n";
    }
    s << "\n";

    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save
// ─────────────────────────────────────────────────────────────────────────────

bool Config::save(const QString& path) {
    const QString target = path.isEmpty() ? m_loadedPath : path;
    if (target.isEmpty()) return false;

    // Create directory if needed
    QDir().mkpath(QFileInfo(target).absolutePath());

    QFile f(target);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[Config] cannot write:" << target;
        return false;
    }

    QTextStream(&f) << serializeTOML();
    qInfo() << "[Config] saved:" << target;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Defaults
// ─────────────────────────────────────────────────────────────────────────────

void Config::setDefaults() {
    mode      = CompositorMode::Tiling;
    cage      = CageConfig{};
    gamescope = GamescopeConfig{};
    theme     = ThemeConfig{};
    anim      = AnimConfig{};
    tiling    = TilingConfig{};
    keys      = KeybindConfig{};
    m_workspaceCount = 9;
}

void Config::loadDefaultKeybinds() {
    keys.modifier = "Super";
    keys.bindings.clear();

    // Applications
    keys.bindings["Return"]       = "exec:alacritty";
    keys.bindings["d"]            = "exec:fuzzel";
    keys.bindings["e"]            = "exec:thunar";
    keys.bindings["b"]            = "exec:firefox";

    // Window management
    keys.bindings["q"]            = "close";
    keys.bindings["f"]            = "fullscreen";
    keys.bindings["Shift+f"]      = "float";
    keys.bindings["m"]            = "maximize";

    // Focus
    keys.bindings["h"]            = "focus:left";
    keys.bindings["l"]            = "focus:right";
    keys.bindings["j"]            = "focus:down";
    keys.bindings["k"]            = "focus:up";
    keys.bindings["Left"]         = "focus:left";
    keys.bindings["Right"]        = "focus:right";
    keys.bindings["Down"]         = "focus:down";
    keys.bindings["Up"]           = "focus:up";

    // Move windows
    keys.bindings["Shift+h"]      = "move:left";
    keys.bindings["Shift+l"]      = "move:right";
    keys.bindings["Shift+j"]      = "move:down";
    keys.bindings["Shift+k"]      = "move:up";

    // Layout cycling
    keys.bindings["Space"]        = "layout:cycle";
    keys.bindings["t"]            = "layout:tiling";
    keys.bindings["Shift+Space"]  = "layout:float";

    // Workspaces 1–9
    for (int i = 1; i <= 9; ++i) {
        keys.bindings[QString::number(i)]             = QString("workspace:%1").arg(i);
        keys.bindings[QString("Shift+%1").arg(i)]     = QString("movetoworkspace:%1").arg(i);
    }

    // System
    keys.bindings["Shift+e"]      = "quit";
    keys.bindings["Shift+r"]      = "reload";
    keys.bindings["Print"]        = "exec:grim";
    keys.bindings["Shift+Print"]  = "exec:grim -g \"$(slurp)\"";

    // Terminal (used by cage/gamescope fallback)
    keys.bindings["terminal"]     = "alacritty";

    qInfo() << "[Config] default keybinds loaded:" << keys.bindings.size() << "bindings";
}
