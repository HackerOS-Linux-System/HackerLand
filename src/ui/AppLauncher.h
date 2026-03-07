#pragma once

#include "GlassWidget.h"

#include <QList>
#include <QHash>
#include <QRect>
#include <QPixmap>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QPropertyAnimation>
#include <QLineEdit>
#include <QThread>
#include <QRunnable>
#include <QFutureWatcher>

class QPainter;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QFocusEvent;

// ─────────────────────────────────────────────────────────────────────────────
// AppCategory
//
// XDG desktop-entry category mapping used to group and filter results.
// ─────────────────────────────────────────────────────────────────────────────
enum class AppCategory {
    All,
    Utility,
    Development,
    Graphics,
    Internet,
    Multimedia,
    Office,
    Settings,
    System,
    Game,
    Other
};

// ─────────────────────────────────────────────────────────────────────────────
// AppEntry
//
// One parsed .desktop file entry.  Loaded once at startup and kept in memory.
// ─────────────────────────────────────────────────────────────────────────────
struct AppEntry {
    // ── Identity ──────────────────────────────────────────────────────────
    QString      name;          ///< Localised application name
    QString      genericName;   ///< e.g. "Web Browser", "Text Editor"
    QString      exec;          ///< Raw Exec= field (may contain %u, %f, etc.)
    QString      execClean;     ///< Exec= with %u/%f/… stripped — ready to run
    QString      tryExec;       ///< Binary to test existence before showing
    QString      iconName;      ///< XDG icon name or absolute path
    QString      description;   ///< Comment= field
    QString      desktopFile;   ///< Absolute path to the .desktop file
    QString      id;            ///< Derived from desktopFile basename (no ext)

    // ── Metadata ──────────────────────────────────────────────────────────
    AppCategory  category  = AppCategory::Other;
    QStringList  keywords;      ///< Keywords= field (for fuzzy matching)
    bool         terminal  = false; ///< Terminal=true → launch in a terminal
    bool         hidden    = false; ///< Hidden=true or NoDisplay=true
    bool         flatpak   = false; ///< Exec= starts with "flatpak run"
    bool         snap      = false; ///< /snap/bin/ path

    // ── Runtime state ─────────────────────────────────────────────────────
    QPixmap      icon;          ///< Loaded lazily; null until first shown
    int          launchCount = 0; ///< Incremented each time the user launches it
    qint64       lastUsed    = 0; ///< Unix timestamp of most recent launch

    // ── Search scoring ────────────────────────────────────────────────────
    int          score       = 0; ///< Computed by scoreEntry(); higher = better
};

// ─────────────────────────────────────────────────────────────────────────────
// LauncherMode
//
// AppLauncher can operate in different modes triggered by the same keybind
// or by a mode-specific keybind.
// ─────────────────────────────────────────────────────────────────────────────
enum class LauncherMode {
    Apps,       ///< Normal application search (default)
    Run,        ///< Direct command entry (like dmenu's run mode)
    Window,     ///< Switch to an open window
    Calc        ///< Inline calculator: "= 2 + 2 * 3"
};

// ─────────────────────────────────────────────────────────────────────────────
// AppLauncher
//
// A full-featured application launcher overlay built on GlassWidget.
//
// Features
//   • Fuzzy search across name, genericName, description, keywords.
//   • Frecency ranking: recently used and frequently used entries rise first.
//   • Category filter tabs below the search field.
//   • Inline calculator when the query starts with "=".
//   • Command-run mode when the query starts with ">".
//   • Window-switch mode: shows open windows when query starts with "@".
//   • Keyboard navigation: ↑/↓/Tab/Enter/Esc.
//   • Mouse navigation: click or hover to select.
//   • Scroll through the result list.
//   • App icons loaded lazily to keep open() fast.
//   • Background blur supplied by WMCompositor via setBackgroundPixmap().
//
// Layout
//   ┌──────────────────────────────────────────────────┐
//   │  🔍  Search applications…                    ×   │  ← search bar
//   │  All │ Dev │ Net │ Media │ Util │ Sys │ …        │  ← category tabs
//   ├──────────────────────────────────────────────────┤
//   │  [icon]  Firefox                                  │
//   │          Web Browser                              │
//   │  [icon]  Alacritty            ← selected         │  ← result rows
//   │          Terminal                                 │
//   │  [icon]  Files                                    │
//   │          …                                        │
//   └──────────────────────────────────────────────────┘
//
// Signals
//   appLaunched(cmd)   — compositor should exec \p cmd
//   windowRaised(id)   — compositor should focus Window with this id
// ─────────────────────────────────────────────────────────────────────────────
class AppLauncher : public GlassWidget {
    Q_OBJECT

    Q_PROPERTY(float showProgress READ showProgress WRITE setShowProgress)

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit AppLauncher(QWidget* parent = nullptr);
    ~AppLauncher() override;

    // ── Open / close ──────────────────────────────────────────────────────

    /// Open the launcher in the given mode, optionally pre-filling the query.
    void open(LauncherMode mode  = LauncherMode::Apps,
              const QString& prefill = {});

    /// Close the launcher with an exit animation.
    void close();

    bool isOpen() const;

    // ── Show-progress property (QPropertyAnimation target) ────────────────
    float showProgress()        const { return m_showProgress; }
    void  setShowProgress(float v)    { m_showProgress = v; update(); }

    // ── Window list (for LauncherMode::Window) ────────────────────────────

    /// Provide the list of open windows so Window mode can show them.
    /// Caller is WMCompositor; should be called before open().
    struct WindowEntry {
        uint64_t id;
        QString  title;
        QString  appId;
        QIcon    icon;
        int      workspaceId;
    };
    void setOpenWindows(const QList<WindowEntry>& windows);

signals:
    /// Compositor should execute this command string.
    void appLaunched(const QString& cmd);

    /// Compositor should focus the window with this id.
    void windowRaised(uint64_t windowId);

protected:
    // ── Qt overrides ──────────────────────────────────────────────────────
    void paintContent    (QPainter&    p) override;
    void keyPressEvent   (QKeyEvent*   e) override;
    void mousePressEvent (QMouseEvent* e) override;
    void mouseMoveEvent  (QMouseEvent* e) override;
    void wheelEvent      (QWheelEvent* e) override;
    void focusOutEvent   (QFocusEvent* e) override;

private slots:
    void onSearchChanged (const QString& text);
    void onAnimTick      ();
    void onIconsLoaded   ();

private:
    // ── Initialisation ────────────────────────────────────────────────────

    /// Parse all .desktop files in XDG_DATA_DIRS/applications and populate
    /// m_allApps.  Called once at construction on a background thread.
    void loadApps();

    /// Parse a single .desktop file and return an AppEntry.
    /// Returns a null AppEntry (hidden=true) if the file should be skipped.
    AppEntry parseDesktopFile(const QString& path) const;

    /// Strip field codes (%u, %f, %U, %F, %i, %c, %k) from an Exec= string.
    static QString cleanExec(const QString& exec);

    /// Map a raw XDG Categories= string to an AppCategory enum value.
    static AppCategory categoryFromString(const QString& cats);

    // ── Search / filtering ────────────────────────────────────────────────

    /// Re-run the search against m_allApps and rebuild m_filtered.
    /// Schedules a repaint; does not modify any Qt widget hierarchy.
    void filterApps(const QString& query);

    /// Compute a relevance score for \p entry against \p query.
    /// Higher = more relevant.  Used to sort m_filtered.
    int  scoreEntry(const AppEntry& entry, const QString& query) const;

    /// Return the frecency weight for \p entry (combines recency + frequency).
    float frecency(const AppEntry& entry) const;

    // ── Calculator mode ───────────────────────────────────────────────────

    /// Evaluate the arithmetic expression in \p expr.
    /// Returns the result as a string, or an empty string on parse error.
    QString evalCalc(const QString& expr) const;

    // ── Layout helpers ────────────────────────────────────────────────────

    /// Rect of the search bar inside the glass widget.
    QRect searchBarRect()   const;

    /// Rect of the category tab row.
    QRect categoryTabRect() const;

    /// Rect of the result list area.
    QRect resultsRect()     const;

    /// Rect of the \p index-th result row (0-based, relative to resultsRect).
    QRect rowRect(int index) const;

    /// Rect of the close (✕) button in the search bar.
    QRect clearButtonRect() const;

    // ── Paint helpers ─────────────────────────────────────────────────────
    void drawSearchBar    (QPainter& p);
    void drawCategoryTabs (QPainter& p);
    void drawResults      (QPainter& p);
    void drawResultRow    (QPainter& p, int index, const AppEntry& entry);
    void drawWindowRow    (QPainter& p, int index, const WindowEntry& entry);
    void drawEmptyState   (QPainter& p);
    void drawCalcResult   (QPainter& p);
    void drawScrollBar    (QPainter& p);

    // ── Icon loading ──────────────────────────────────────────────────────

    /// Load (or return cached) icon for \p iconName at \p size.
    QPixmap iconForName(const QString& iconName, int size = kIconSize) const;

    /// Trigger lazy loading of icons for all currently-visible rows.
    void loadVisibleIcons();

    // ── Navigation ────────────────────────────────────────────────────────
    void selectNext  ();
    void selectPrev  ();
    void selectIndex (int index);
    void confirmSelection();

    /// Number of visible result rows (capped by resultsRect height).
    int  visibleRowCount() const;

    // ── Frecency persistence ──────────────────────────────────────────────

    /// Load launch counts and timestamps from the cache file.
    void loadFrecency();

    /// Save launch counts and timestamps to the cache file.
    void saveFrecency() const;

    /// Record a launch of \p entry (increments count, updates timestamp).
    void recordLaunch(AppEntry& entry);

    // ── Helpers ───────────────────────────────────────────────────────────
    QFont  searchFont()   const;
    QFont  resultFont()   const;
    QFont  subFont()      const;
    QFont  tabFont()      const;

    static QString frecencyCachePath();

    // ── Members ───────────────────────────────────────────────────────────

    // Application data
    QList<AppEntry>    m_allApps;       ///< All parsed .desktop entries
    QList<AppEntry>    m_filtered;      ///< Current filtered+sorted subset
    QList<WindowEntry> m_openWindows;   ///< For LauncherMode::Window

    // Mode and state
    LauncherMode       m_mode          = LauncherMode::Apps;
    QString            m_query;
    AppCategory        m_activeCategory= AppCategory::All;
    QString            m_calcResult;   ///< Non-empty when in calc mode

    // Selection
    int                m_selectedIndex = 0;
    int                m_scrollOffset  = 0;  ///< First visible row index

    // Hover
    int                m_hoveredIndex  = -1;

    // Inline search field (logical; drawn manually, not a QLineEdit widget)
    int                m_cursorPos     = 0;  ///< Caret position in m_query
    int                m_selStart      = -1; ///< Selection start (-1 = none)
    int                m_selEnd        = -1;

    // Animation
    float              m_showProgress  = 0.f;
    QPropertyAnimation* m_showAnim     = nullptr;
    QTimer*            m_animTimer     = nullptr;
    float              m_cursorBlink   = 1.f; ///< 0/1 blink phase
    float              m_blinkPhase    = 0.f; ///< accumulator (0→1→0)

    // Icon cache
    mutable QHash<QString, QPixmap> m_iconCache;

    // Frecency
    QHash<QString, int>    m_launchCounts;   ///< desktop-id → count
    QHash<QString, qint64> m_lastUsedMap;    ///< desktop-id → timestamp

    // ── Layout constants ──────────────────────────────────────────────────
    static constexpr int   kWidth           = 660;  ///< Widget width  px
    static constexpr int   kHeight          = 520;  ///< Widget height px
    static constexpr int   kPadH            =  20;  ///< Horizontal padding
    static constexpr int   kPadV            =  18;  ///< Vertical padding
    static constexpr int   kSearchH         =  48;  ///< Search bar height
    static constexpr int   kTabH            =  32;  ///< Category tab row height
    static constexpr int   kRowH            =  52;  ///< Result row height
    static constexpr int   kIconSize        =  32;  ///< Icon square size
    static constexpr int   kIconMargin      =  10;  ///< Icon left margin
    static constexpr int   kScrollBarW      =   4;  ///< Scroll bar width
    static constexpr int   kSearchRadius    =  10;  ///< Search bar corner radius
    static constexpr int   kRowRadius       =   8;  ///< Row hover corner radius

    // ── Animation constants ───────────────────────────────────────────────
    static constexpr int   kShowMs          = 160;  ///< Open animation ms
    static constexpr int   kHideMs          = 120;  ///< Close animation ms
    static constexpr float kHoverSpeed      = 0.14f;///< Per-tick hover fade
    static constexpr float kBlinkInterval   = 0.016f;///< ~60 fps per tick
};
