#include "Config.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDebug>
#include <QColor>

// ─────────────────────────────────────────────────────────────────────────────
// JSON helpers — anonymous namespace
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    /// Read a string field with a fallback default.
    QString jStr(const QJsonObject& obj, const char* key, const QString& def) {
        auto it = obj.constFind(key);
        if (it == obj.constEnd() || !it->isString()) return def;
        return it->toString();
    }

    /// Read an int field with a fallback default.
    int jInt(const QJsonObject& obj, const char* key, int def) {
        auto it = obj.constFind(key);
        if (it == obj.constEnd()) return def;
        if (it->isDouble()) return it->toInt(def);
        if (it->isString()) {
            bool ok = false;
            int v = it->toString().toInt(&ok);
            return ok ? v : def;
        }
        return def;
    }

    /// Read a double/float field with a fallback default.
    double jDouble(const QJsonObject& obj, const char* key, double def) {
        auto it = obj.constFind(key);
        if (it == obj.constEnd()) return def;
        if (it->isDouble()) return it->toDouble(def);
        if (it->isString()) {
            bool ok = false;
            double v = it->toString().toDouble(&ok);
            return ok ? v : def;
        }
        return def;
    }

    /// Read a bool field with a fallback default.
    bool jBool(const QJsonObject& obj, const char* key, bool def) {
        auto it = obj.constFind(key);
        if (it == obj.constEnd()) return def;
        if (it->isBool())   return it->toBool(def);
        if (it->isDouble()) return it->toInt() != 0;
        if (it->isString()) {
            const QString s = it->toString().toLower().trimmed();
            if (s == "true"  || s == "1" || s == "yes" || s == "on")  return true;
            if (s == "false" || s == "0" || s == "no"  || s == "off") return false;
        }
        return def;
    }

    /// Read a QColor field from a CSS hex string (#RRGGBB or #AARRGGBB).
    /// Falls back to \p def if the field is missing or unparseable.
    QColor jColor(const QJsonObject& obj, const char* key, const QColor& def) {
        auto it = obj.constFind(key);
        if (it == obj.constEnd() || !it->isString()) return def;
        QColor c(it->toString());
        return c.isValid() ? c : def;
    }

    /// Serialise a QColor to "#AARRGGBB" (always includes alpha so transparency
    /// round-trips correctly).
    QString colorToJson(const QColor& c) {
        return c.name(QColor::HexArgb);
    }

    /// Return the user-specific config path, creating the directory if needed.
    QString userConfigPath() {
        // ~/.config/hackerlandwm/config.json
        const QString dir = QDir::homePath() + "/.config/hackerlandwm";
        QDir().mkpath(dir);
        return dir + "/config.json";
    }

    /// Return the system-wide fallback config path.
    QString systemConfigPath() {
        return "/etc/hackerlandwm/hackerlandwm.conf";
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

Config& Config::instance() {
    static Config inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Config::Config() {
    // Struct initialisers already set all defaults — setDefaults() is a
    // no-op in the common case but available for explicit reset.
    setDefaults();
    loadDefaultKeybinds();
}

// ─────────────────────────────────────────────────────────────────────────────
// setDefaults — reset every field to the compiled-in values
// ─────────────────────────────────────────────────────────────────────────────

void Config::setDefaults() {
    // ── Theme ─────────────────────────────────────────────────────────────
    theme.accentColor       = QColor(120, 200, 255, 230);
    theme.accentSecondary   = QColor(180, 120, 255, 200);
    theme.accentTertiary    = QColor( 80, 255, 200, 180);

    theme.glassBackground   = QColor( 10,  15,  30, 160);
    theme.glassBorder       = QColor(255, 255, 255,  40);
    theme.glassBorderActive = QColor(120, 200, 255, 120);

    theme.textPrimary       = QColor(240, 245, 255, 255);
    theme.textSecondary     = QColor(180, 190, 210, 200);
    theme.textMuted         = QColor(120, 130, 155, 160);
    theme.shadowColor       = QColor(  0,   5,  20, 200);

    theme.barBackground     = QColor(  8,  12,  25, 200);
    theme.barBorder         = QColor(255, 255, 255,  20);
    theme.barHeight         = 38;
    theme.barBlur           = true;

    theme.borderWidth       = 2;
    theme.borderRadius      = 12;
    theme.gapInner          = 10;
    theme.gapOuter          = 12;
    theme.inactiveOpacity   = 0.92f;
    theme.activeOpacity     = 1.0f;
    theme.blurRadius        = 24.0f;
    theme.blurSaturation    = 1.3f;

    theme.fontFamily        = "Geist";
    theme.fontFallback      = "SF Pro Display, Inter, sans-serif";
    theme.fontSizeBar       = 13;
    theme.fontSizeUI        = 12;

    theme.wallpaperPath     = "/usr/share/wallpapers/HackerOS-Wallpapers/Wallpaper24.png";
    theme.wallpaperMode     = "fill";

    // ── Animation ─────────────────────────────────────────────────────────
    anim.enabled            = true;
    anim.windowOpenMs       = 280;
    anim.windowCloseMs      = 220;
    anim.workspaceSwitchMs  = 320;
    anim.tileRearrangeMs    = 250;
    anim.openEasing         = "cubic-bezier(0.34,1.56,0.64,1)";
    anim.closeEasing        = "cubic-bezier(0.55,0,1,0.45)";
    anim.scaleFactor        = 0.92f;

    // ── Tiling ────────────────────────────────────────────────────────────
    tiling.layout           = "spiral";
    tiling.masterRatio      = 0.55f;
    tiling.maxColumns       = 3;
    tiling.smartGaps        = true;
    tiling.smartBorders     = true;
    tiling.centerSingle     = true;
    tiling.centerSingleScale = 0.72f;

    // ── Keys ──────────────────────────────────────────────────────────────
    keys.modifier           = "Super";
    keys.bindings.clear();

    // ── Meta ──────────────────────────────────────────────────────────────
    m_workspaceCount        = 9;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadDefaultKeybinds
// ─────────────────────────────────────────────────────────────────────────────

void Config::loadDefaultKeybinds() {
    const QString& m = keys.modifier; // "Super"

    // ── Application launchers ─────────────────────────────────────────────
    keys.bindings[m + "+Return"]       = "exec:alacritty";
    keys.bindings[m + "+Space"]        = "launcher";
    keys.bindings[m + "+E"]            = "exec:thunar";
    keys.bindings[m + "+B"]            = "exec:firefox";
    keys.bindings[m + "+Shift+B"]      = "exec:chromium";

    // ── Window control ────────────────────────────────────────────────────
    keys.bindings[m + "+Q"]            = "close";
    keys.bindings[m + "+F"]            = "fullscreen";
    keys.bindings[m + "+Shift+F"]      = "float_toggle";
    keys.bindings[m + "+Shift+M"]      = "maximize_toggle";
    keys.bindings[m + "+P"]            = "pin";
    keys.bindings[m + "+N"]            = "minimize";

    // ── Focus — vi-keys ───────────────────────────────────────────────────
    keys.bindings[m + "+H"]            = "focus:left";
    keys.bindings[m + "+J"]            = "focus:down";
    keys.bindings[m + "+K"]            = "focus:up";
    keys.bindings[m + "+L"]            = "focus:right";
    // Focus — arrow keys
    keys.bindings[m + "+Left"]         = "focus:left";
    keys.bindings[m + "+Down"]         = "focus:down";
    keys.bindings[m + "+Up"]           = "focus:up";
    keys.bindings[m + "+Right"]        = "focus:right";
    // Focus — cycle
    keys.bindings[m + "+Tab"]          = "focus:next";
    keys.bindings[m + "+Shift+Tab"]    = "focus:prev";
    keys.bindings[m + "+grave"]        = "focus:last";     // Super+` = alt-tab style
    keys.bindings[m + "+Return"]       = "focus:master";   // override: also promote

    // ── Move windows — vi-keys ────────────────────────────────────────────
    keys.bindings[m + "+Shift+H"]      = "move:left";
    keys.bindings[m + "+Shift+J"]      = "move:down";
    keys.bindings[m + "+Shift+K"]      = "move:up";
    keys.bindings[m + "+Shift+L"]      = "move:right";
    // Move to master position
    keys.bindings[m + "+Shift+Return"] = "move:master";

    // ── Master ratio ──────────────────────────────────────────────────────
    keys.bindings[m + "+Ctrl+H"]       = "master_ratio:shrink";
    keys.bindings[m + "+Ctrl+L"]       = "master_ratio:grow";
    keys.bindings[m + "+Ctrl+Left"]    = "master_ratio:shrink";
    keys.bindings[m + "+Ctrl+Right"]   = "master_ratio:grow";

    // ── Window resize (floating) ──────────────────────────────────────────
    keys.bindings[m + "+Ctrl+Shift+H"] = "resize:shrink_width";
    keys.bindings[m + "+Ctrl+Shift+L"] = "resize:grow_width";
    keys.bindings[m + "+Ctrl+Shift+J"] = "resize:grow_height";
    keys.bindings[m + "+Ctrl+Shift+K"] = "resize:shrink_height";

    // ── Layout selection ──────────────────────────────────────────────────
    keys.bindings[m + "+T"]            = "layout:spiral";
    keys.bindings[m + "+Shift+T"]      = "layout:tall";
    keys.bindings[m + "+W"]            = "layout:wide";
    keys.bindings[m + "+G"]            = "layout:grid";
    keys.bindings[m + "+D"]            = "layout:dwindle";
    keys.bindings[m + "+M"]            = "layout:monocle";
    keys.bindings[m + "+Shift+C"]      = "layout:centered";
    keys.bindings[m + "+Shift+G"]      = "layout:three_column";
    // Cycle forward / backward through layouts
    keys.bindings[m + "+bracketright"] = "layout:cycle_forward";
    keys.bindings[m + "+bracketleft"]  = "layout:cycle_backward";

    // ── Workspace switching 1–9 ───────────────────────────────────────────
    for (int i = 1; i <= 9; ++i) {
        const QString n = QString::number(i);
        keys.bindings[m + "+" + n]           = "workspace:" + n;
        keys.bindings[m + "+Shift+" + n]     = "move_to_workspace:" + n;
        keys.bindings[m + "+Ctrl+" + n]      = "move_to_workspace_follow:" + n;
    }
    // Workspace cycling
    keys.bindings[m + "+Ctrl+Right"]   = "workspace:next";
    keys.bindings[m + "+Ctrl+Left"]    = "workspace:prev";
    keys.bindings[m + "+Shift+Right"]  = "workspace:next";
    keys.bindings[m + "+Shift+Left"]   = "workspace:prev";

    // ── Screenshots ───────────────────────────────────────────────────────
    keys.bindings["Print"]             = "exec:grim";
    keys.bindings[m + "+Print"]        = "exec:grim -g \"$(slurp)\"";
    keys.bindings[m + "+Shift+Print"]  = "exec:grim -g \"$(slurp)\" - | wl-copy";

    // ── System ────────────────────────────────────────────────────────────
    keys.bindings[m + "+Shift+R"]      = "reload_config";
    keys.bindings[m + "+Shift+Q"]      = "quit";
    keys.bindings[m + "+Shift+E"]      = "logout";
    keys.bindings[m + "+Ctrl+L"]       = "exec:swaylock";
    keys.bindings["XF86AudioRaiseVolume"]  = "exec:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+";
    keys.bindings["XF86AudioLowerVolume"]  = "exec:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-";
    keys.bindings["XF86AudioMute"]         = "exec:wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle";
    keys.bindings["XF86AudioPlay"]         = "exec:playerctl play-pause";
    keys.bindings["XF86AudioNext"]         = "exec:playerctl next";
    keys.bindings["XF86AudioPrev"]         = "exec:playerctl previous";
    keys.bindings["XF86MonBrightnessUp"]   = "exec:brightnessctl set 10%+";
    keys.bindings["XF86MonBrightnessDown"] = "exec:brightnessctl set 10%-";

    qDebug() << "[Config] default keybinds loaded:"
    << keys.bindings.size() << "bindings";
}

// ─────────────────────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────────────────────

bool Config::load(const QString& path) {
    // Resolve the file to load.  If \p path is empty, try user config first,
    // then fall back to the system-wide config.
    QString filePath = path;
    if (filePath.isEmpty()) {
        filePath = userConfigPath();
        if (!QFile::exists(filePath)) {
            filePath = systemConfigPath();
        }
    }

    if (!QFile::exists(filePath)) {
        qWarning() << "[Config] file not found:" << filePath
        << "— using compiled-in defaults";
        m_loadedPath = filePath;
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[Config] cannot open" << filePath
        << ":" << file.errorString();
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "[Config] JSON parse error in" << filePath
        << "at offset" << err.offset
        << ":" << err.errorString();
        return false;
    }

    if (!doc.isObject()) {
        qWarning() << "[Config] root is not a JSON object in" << filePath;
        return false;
    }

    const QJsonObject root = doc.object();

    // ── Parse [theme] ─────────────────────────────────────────────────────
    if (root.contains("theme") && root["theme"].isObject()) {
        const QJsonObject t = root["theme"].toObject();

        theme.accentColor       = jColor(t, "accent",           theme.accentColor);
        theme.accentSecondary   = jColor(t, "accent_secondary",  theme.accentSecondary);
        theme.accentTertiary    = jColor(t, "accent_tertiary",   theme.accentTertiary);
        theme.glassBackground   = jColor(t, "glass_background",  theme.glassBackground);
        theme.glassBorder       = jColor(t, "glass_border",      theme.glassBorder);
        theme.glassBorderActive = jColor(t, "glass_border_active",theme.glassBorderActive);
        theme.textPrimary       = jColor(t, "text_primary",      theme.textPrimary);
        theme.textSecondary     = jColor(t, "text_secondary",    theme.textSecondary);
        theme.textMuted         = jColor(t, "text_muted",        theme.textMuted);
        theme.shadowColor       = jColor(t, "shadow_color",      theme.shadowColor);
        theme.barBackground     = jColor(t, "bar_background",    theme.barBackground);
        theme.barBorder         = jColor(t, "bar_border",        theme.barBorder);

        theme.barHeight         = jInt   (t, "bar_height",       theme.barHeight);
        theme.barBlur           = jBool  (t, "bar_blur",         theme.barBlur);
        theme.borderWidth       = jInt   (t, "border_width",     theme.borderWidth);
        theme.borderRadius      = jInt   (t, "border_radius",    theme.borderRadius);
        theme.gapInner          = jInt   (t, "gap_inner",        theme.gapInner);
        theme.gapOuter          = jInt   (t, "gap_outer",        theme.gapOuter);
        theme.inactiveOpacity   = (float)jDouble(t,"inactive_opacity",theme.inactiveOpacity);
        theme.activeOpacity     = (float)jDouble(t,"active_opacity",  theme.activeOpacity);
        theme.blurRadius        = (float)jDouble(t,"blur_radius",     theme.blurRadius);
        theme.blurSaturation    = (float)jDouble(t,"blur_saturation", theme.blurSaturation);

        theme.fontFamily        = jStr   (t, "font_family",      theme.fontFamily);
        theme.fontFallback      = jStr   (t, "font_fallback",    theme.fontFallback);
        theme.fontSizeBar       = jInt   (t, "font_size_bar",    theme.fontSizeBar);
        theme.fontSizeUI        = jInt   (t, "font_size_ui",     theme.fontSizeUI);

        theme.wallpaperPath     = jStr   (t, "wallpaper",        theme.wallpaperPath);
        theme.wallpaperMode     = jStr   (t, "wallpaper_mode",   theme.wallpaperMode);
    }

    // ── Parse [animations] ────────────────────────────────────────────────
    if (root.contains("animations") && root["animations"].isObject()) {
        const QJsonObject a = root["animations"].toObject();

        anim.enabled           = jBool  (a, "enabled",          anim.enabled);
        anim.windowOpenMs      = jInt   (a, "window_open_ms",   anim.windowOpenMs);
        anim.windowCloseMs     = jInt   (a, "window_close_ms",  anim.windowCloseMs);
        anim.workspaceSwitchMs = jInt   (a, "workspace_switch_ms", anim.workspaceSwitchMs);
        anim.tileRearrangeMs   = jInt   (a, "tile_rearrange_ms",anim.tileRearrangeMs);
        anim.openEasing        = jStr   (a, "open_easing",      anim.openEasing);
        anim.closeEasing       = jStr   (a, "close_easing",     anim.closeEasing);
        anim.scaleFactor       = (float)jDouble(a,"scale_factor",anim.scaleFactor);
    }

    // ── Parse [tiling] ────────────────────────────────────────────────────
    if (root.contains("tiling") && root["tiling"].isObject()) {
        const QJsonObject t = root["tiling"].toObject();

        tiling.layout            = jStr   (t, "layout",             tiling.layout);
        tiling.masterRatio       = (float)jDouble(t,"master_ratio", tiling.masterRatio);
        tiling.maxColumns        = jInt   (t, "max_columns",        tiling.maxColumns);
        tiling.smartGaps         = jBool  (t, "smart_gaps",         tiling.smartGaps);
        tiling.smartBorders      = jBool  (t, "smart_borders",      tiling.smartBorders);
        tiling.centerSingle      = jBool  (t, "center_single",      tiling.centerSingle);
        tiling.centerSingleScale = (float)jDouble(t,"center_single_scale",
                                                  tiling.centerSingleScale);
    }

    // ── Parse [workspaces] ────────────────────────────────────────────────
    if (root.contains("workspaces") && root["workspaces"].isObject()) {
        const QJsonObject w = root["workspaces"].toObject();
        m_workspaceCount = jInt(w, "count", m_workspaceCount);
        m_workspaceCount = qBound(1, m_workspaceCount, 20);
    }

    // ── Parse [keybinds] ─────────────────────────────────────────────────
    // The config file's [keybinds] section merges with (and can override)
    // the defaults populated by loadDefaultKeybinds().  User bindings take
    // precedence; use the special value "none" or "" to unbind a key.
    if (root.contains("keybinds") && root["keybinds"].isObject()) {
        const QJsonObject kb = root["keybinds"].toObject();

        // Optional: override the modifier key.
        if (kb.contains("modifier") && kb["modifier"].isString()) {
            keys.modifier = kb["modifier"].toString();
        }

        // Merge binding entries.
        const QJsonObject binds = kb.contains("bindings") && kb["bindings"].isObject()
        ? kb["bindings"].toObject()
        : kb; // legacy: flat object at [keybinds] level

        for (auto it = binds.constBegin(); it != binds.constEnd(); ++it) {
            if (it.key() == "modifier") continue;
            const QString action = it->toString().trimmed();
            if (action.isEmpty() || action.toLower() == "none") {
                keys.bindings.remove(it.key());
            } else {
                keys.bindings[it.key()] = action;
            }
        }
    }

    m_loadedPath = filePath;

    qInfo() << "[Config] loaded from:" << filePath
    << "| bindings:" << keys.bindings.size()
    << "| workspaces:" << m_workspaceCount;

    emit configReloaded();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────────────

bool Config::save(const QString& path) {
    const QString savePath = path.isEmpty()
    ? (m_loadedPath.isEmpty() ? userConfigPath() : m_loadedPath)
    : path;

    // Create the parent directory if it doesn't exist.
    QDir().mkpath(QFileInfo(savePath).absoluteDir().absolutePath());

    // ── Build JSON document ───────────────────────────────────────────────

    QJsonObject root;

    // ── [theme] ───────────────────────────────────────────────────────────
    {
        QJsonObject t;
        t["accent"]              = colorToJson(theme.accentColor);
        t["accent_secondary"]    = colorToJson(theme.accentSecondary);
        t["accent_tertiary"]     = colorToJson(theme.accentTertiary);
        t["glass_background"]    = colorToJson(theme.glassBackground);
        t["glass_border"]        = colorToJson(theme.glassBorder);
        t["glass_border_active"] = colorToJson(theme.glassBorderActive);
        t["text_primary"]        = colorToJson(theme.textPrimary);
        t["text_secondary"]      = colorToJson(theme.textSecondary);
        t["text_muted"]          = colorToJson(theme.textMuted);
        t["shadow_color"]        = colorToJson(theme.shadowColor);
        t["bar_background"]      = colorToJson(theme.barBackground);
        t["bar_border"]          = colorToJson(theme.barBorder);

        t["bar_height"]          = theme.barHeight;
        t["bar_blur"]            = theme.barBlur;
        t["border_width"]        = theme.borderWidth;
        t["border_radius"]       = theme.borderRadius;
        t["gap_inner"]           = theme.gapInner;
        t["gap_outer"]           = theme.gapOuter;
        t["inactive_opacity"]    = (double)theme.inactiveOpacity;
        t["active_opacity"]      = (double)theme.activeOpacity;
        t["blur_radius"]         = (double)theme.blurRadius;
        t["blur_saturation"]     = (double)theme.blurSaturation;

        t["font_family"]         = theme.fontFamily;
        t["font_fallback"]       = theme.fontFallback;
        t["font_size_bar"]       = theme.fontSizeBar;
        t["font_size_ui"]        = theme.fontSizeUI;

        t["wallpaper"]           = theme.wallpaperPath;
        t["wallpaper_mode"]      = theme.wallpaperMode;

        root["theme"] = t;
    }

    // ── [animations] ──────────────────────────────────────────────────────
    {
        QJsonObject a;
        a["enabled"]             = anim.enabled;
        a["window_open_ms"]      = anim.windowOpenMs;
        a["window_close_ms"]     = anim.windowCloseMs;
        a["workspace_switch_ms"] = anim.workspaceSwitchMs;
        a["tile_rearrange_ms"]   = anim.tileRearrangeMs;
        a["open_easing"]         = anim.openEasing;
        a["close_easing"]        = anim.closeEasing;
        a["scale_factor"]        = (double)anim.scaleFactor;
        root["animations"] = a;
    }

    // ── [tiling] ──────────────────────────────────────────────────────────
    {
        QJsonObject t;
        t["layout"]              = tiling.layout;
        t["master_ratio"]        = (double)tiling.masterRatio;
        t["max_columns"]         = tiling.maxColumns;
        t["smart_gaps"]          = tiling.smartGaps;
        t["smart_borders"]       = tiling.smartBorders;
        t["center_single"]       = tiling.centerSingle;
        t["center_single_scale"] = (double)tiling.centerSingleScale;
        root["tiling"] = t;
    }

    // ── [workspaces] ──────────────────────────────────────────────────────
    {
        QJsonObject w;
        w["count"] = m_workspaceCount;
        root["workspaces"] = w;
    }

    // ── [keybinds] ────────────────────────────────────────────────────────
    {
        QJsonObject kb;
        kb["modifier"] = keys.modifier;

        QJsonObject binds;
        for (auto it = keys.bindings.constBegin();
             it != keys.bindings.constEnd(); ++it)
             {
                 binds[it.key()] = it.value();
             }
             kb["bindings"] = binds;
        root["keybinds"] = kb;
    }

    // ── Write file ────────────────────────────────────────────────────────
    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[Config] cannot write to" << savePath
        << ":" << file.errorString();
        return false;
    }

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        qWarning() << "[Config] short write to" << savePath;
        file.close();
        return false;
    }

    file.close();
    m_loadedPath = savePath;

    qInfo() << "[Config] saved to:" << savePath;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// reload — convenience: re-load from the last-used path
// ─────────────────────────────────────────────────────────────────────────────

bool Config::reload() {
    const QString path = m_loadedPath.isEmpty() ? userConfigPath() : m_loadedPath;
    qInfo() << "[Config] reloading from:" << path;
    setDefaults();
    loadDefaultKeybinds();
    return load(path);
}
