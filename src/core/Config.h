#pragma once
#include <QObject>
#include <QString>
#include <QColor>
#include <QMap>
#include <QVariant>
#include <QKeySequence>
#include <QStringList>

struct ThemeConfig {
    // Glassmorphism colors
    QColor accentColor        = QColor(120, 200, 255, 230);
    QColor accentSecondary    = QColor(180, 120, 255, 200);
    QColor accentTertiary     = QColor(80, 255, 200, 180);

    QColor glassBackground    = QColor(10, 15, 30, 160);
    QColor glassBorder        = QColor(255, 255, 255, 40);
    QColor glassBorderActive  = QColor(120, 200, 255, 120);

    QColor textPrimary        = QColor(240, 245, 255, 255);
    QColor textSecondary      = QColor(180, 190, 210, 200);
    QColor textMuted          = QColor(120, 130, 155, 160);

    QColor shadowColor        = QColor(0, 5, 20, 200);

    // Bar
    QColor barBackground      = QColor(8, 12, 25, 200);
    QColor barBorder          = QColor(255, 255, 255, 20);
    int    barHeight          = 38;
    bool   barBlur            = true;

    // Window decorations
    int    borderWidth        = 2;
    int    borderRadius       = 12;
    int    gapInner           = 10;
    int    gapOuter           = 12;
    float  inactiveOpacity    = 0.92f;
    float  activeOpacity      = 1.0f;
    float  blurRadius         = 24.0f;
    float  blurSaturation     = 1.3f;

    // Fonts
    QString fontFamily        = "Geist";
    QString fontFallback      = "SF Pro Display, Inter, sans-serif";
    int    fontSizeBar        = 13;
    int    fontSizeUI         = 12;

    // Wallpaper
    QString wallpaperPath     = "/usr/share/wallpapers/HackerOS-Wallpapers/Wallpaper24.png";
    QString wallpaperMode     = "fill"; // fill, fit, center, tile
};

struct AnimConfig {
    bool   enabled            = true;
    int    windowOpenMs       = 280;
    int    windowCloseMs      = 220;
    int    workspaceSwitchMs  = 320;
    int    tileRearrangeMs    = 250;
    QString openEasing        = "cubic-bezier(0.34,1.56,0.64,1)"; // spring
    QString closeEasing       = "cubic-bezier(0.55,0,1,0.45)";
    float  scaleFactor        = 0.92f; // scale on open
};

struct TilingConfig {
    QString layout            = "spiral"; // spiral, tall, wide, grid, dwindle, monocle
    float   masterRatio       = 0.55f;
    int     maxColumns        = 3;
    bool    smartGaps         = true;
    bool    smartBorders      = true; // remove borders when single window
    bool    centerSingle      = true;
    float   centerSingleScale = 0.72f;
};

struct KeybindConfig {
    QString modifier          = "Super"; // Super, Alt, Ctrl
    // Actions mapped by key combo string
    QMap<QString, QString> bindings;
};

class Config : public QObject {
    Q_OBJECT
public:
    static Config& instance();

    bool load(const QString& path = QString());
    bool reload();
    bool save(const QString& path = QString());
    void setDefaults();

    ThemeConfig   theme;
    AnimConfig    anim;
    TilingConfig  tiling;
    KeybindConfig keys;

    int  workspaceCount() const { return m_workspaceCount; }
    bool animationsEnabled() const { return anim.enabled; }
    void setAnimationsEnabled(bool e) { anim.enabled = e; }

signals:
    void configReloaded();
    void themeChanged();

private:
    Config();
    void loadDefaultKeybinds();

    int m_workspaceCount = 9;
    QString m_loadedPath;
};
