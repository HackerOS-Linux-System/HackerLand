#include "AppLauncher.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QRegularExpression>
#include <QLinearGradient>
#include <QFontMetrics>
#include <QDateTime>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QtMath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

AppLauncher::AppLauncher(QWidget* parent)
: GlassWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
    Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::StrongFocus);

    // Centre on primary screen
    if (auto* screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry();
        setGeometry(sg.x() + (sg.width()  - kWidth)  / 2,
                    sg.y() + 120,
                    kWidth, kHeight);
    } else {
        resize(kWidth, kHeight);
    }

    // Animation timer (~60 fps)
    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, &AppLauncher::onAnimTick);
    m_animTimer->start(16);

    loadFrecency();
    loadApps();       // populates m_allApps synchronously (background-thread-safe)
    filterApps({});
    hide();
}

AppLauncher::~AppLauncher() {
    saveFrecency();
}

// ─────────────────────────────────────────────────────────────────────────────
// Open / close
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::open(LauncherMode mode, const QString& prefill) {
    m_mode          = mode;
    m_query         = prefill;
    m_cursorPos     = prefill.length();
    m_selectedIndex = 0;
    m_scrollOffset  = 0;
    m_calcResult.clear();

    filterApps(m_query);

    if (!isVisible()) {
        m_showProgress = 0.f;
        show();
        raise();
        setFocus();
    }
}

void AppLauncher::close() {
    hide();
    m_query.clear();
    m_cursorPos = 0;
}

bool AppLauncher::isOpen() const { return isVisible(); }

void AppLauncher::setOpenWindows(const QList<WindowEntry>& windows) {
    m_openWindows = windows;
}

// ─────────────────────────────────────────────────────────────────────────────
// App loading
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::loadApps() {
    m_allApps.clear();                          // m_allApps (not m_apps)

    const QStringList dirs = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        QDir::homePath() + "/.local/share/applications"
    };

    for (const auto& dirPath : dirs) {
        QDir d(dirPath);
        if (!d.exists()) continue;
        for (const auto& file : d.entryList({"*.desktop"}, QDir::Files)) {
            AppEntry entry = parseDesktopFile(d.filePath(file));
            if (!entry.hidden && !entry.name.isEmpty() && !entry.execClean.isEmpty()) {
                // Restore frecency data
                entry.launchCount = m_launchCounts.value(entry.id, 0);
                entry.lastUsed    = m_lastUsedMap.value(entry.id, 0);
                m_allApps.append(entry);
            }
        }
    }

    std::sort(m_allApps.begin(), m_allApps.end(),
              [](const AppEntry& a, const AppEntry& b) {
                  return a.name.toLower() < b.name.toLower();
              });
}

AppEntry AppLauncher::parseDesktopFile(const QString& path) const {
    AppEntry e;
    e.desktopFile = path;
    e.id          = QFileInfo(path).completeBaseName();
    e.hidden      = true;  // default — mark visible only if all checks pass

    QSettings s(path, QSettings::IniFormat);
    s.beginGroup("Desktop Entry");

    if (s.value("Type").toString() != "Application")         return e;
    if (s.value("NoDisplay", false).toBool())                return e;
    if (s.value("Hidden",    false).toBool())                return e;

    e.name        = s.value("Name").toString();
    e.genericName = s.value("GenericName").toString();
    e.description = s.value("Comment").toString();
    e.iconName    = s.value("Icon").toString();
    e.tryExec     = s.value("TryExec").toString();
    e.terminal    = s.value("Terminal", false).toBool();
    e.keywords    = s.value("Keywords").toString().split(';', Qt::SkipEmptyParts);

    const QString rawExec = s.value("Exec").toString();
    e.exec        = rawExec;
    e.execClean   = cleanExec(rawExec);

    const QString cats = s.value("Categories").toString();
    e.category = categoryFromString(cats);

    e.flatpak = e.execClean.startsWith("flatpak");
    e.snap    = e.execClean.startsWith("/snap/");

    if (!e.name.isEmpty() && !e.execClean.isEmpty())
        e.hidden = false;

    s.endGroup();
    return e;
}

QString AppLauncher::cleanExec(const QString& exec) {
    QString r = exec;
    r.remove(QRegularExpression("%[a-zA-Z]"));  // strip field codes
    return r.trimmed();
}

AppCategory AppLauncher::categoryFromString(const QString& cats) {
    if (cats.contains("Development", Qt::CaseInsensitive)) return AppCategory::Development;
    if (cats.contains("Network",     Qt::CaseInsensitive)) return AppCategory::Internet;
    if (cats.contains("Graphics",    Qt::CaseInsensitive)) return AppCategory::Graphics;
    if (cats.contains("Audio",       Qt::CaseInsensitive)) return AppCategory::Multimedia;
    if (cats.contains("Video",       Qt::CaseInsensitive)) return AppCategory::Multimedia;
    if (cats.contains("Office",      Qt::CaseInsensitive)) return AppCategory::Office;
    if (cats.contains("Settings",    Qt::CaseInsensitive)) return AppCategory::Settings;
    if (cats.contains("System",      Qt::CaseInsensitive)) return AppCategory::System;
    if (cats.contains("Utility",     Qt::CaseInsensitive)) return AppCategory::Utility;
    if (cats.contains("Game",        Qt::CaseInsensitive)) return AppCategory::Game;
    return AppCategory::Other;
}

// ─────────────────────────────────────────────────────────────────────────────
// Search / filtering
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::filterApps(const QString& query) {
    m_query = query;
    m_filtered.clear();
    m_selectedIndex = 0;
    m_scrollOffset  = 0;
    m_calcResult.clear();

    // Calculator mode
    if (query.startsWith('=')) {
        m_calcResult = evalCalc(query.mid(1).trimmed());
        update();
        return;
    }

    for (auto& app : m_allApps) {
        if (m_activeCategory != AppCategory::All &&
            app.category != m_activeCategory) continue;

        app.score = scoreEntry(app, query);
        if (app.score > 0 || query.isEmpty())
            m_filtered.append(app);

        if (m_filtered.size() >= 100) break;
    }

    std::stable_sort(m_filtered.begin(), m_filtered.end(),
                     [](const AppEntry& a, const AppEntry& b) {
                         return a.score > b.score;
                     });

    loadVisibleIcons();
    update();
}

int AppLauncher::scoreEntry(const AppEntry& entry, const QString& query) const {
    if (query.isEmpty()) return 1 + (int)(frecency(entry) * 100);

    int score = 0;
    if (entry.name.startsWith(query, Qt::CaseInsensitive))        score += 100;
    else if (entry.name.contains(query, Qt::CaseInsensitive))     score +=  60;
    if (entry.genericName.contains(query, Qt::CaseInsensitive))   score +=  30;
    if (entry.description.contains(query, Qt::CaseInsensitive))   score +=  15;
    for (const auto& kw : entry.keywords) {
        if (kw.contains(query, Qt::CaseInsensitive))              score +=  10;
    }
    if (entry.execClean.contains(query, Qt::CaseInsensitive))     score +=   5;

    if (score > 0) score += (int)(frecency(entry) * 20);
    return score;
}

float AppLauncher::frecency(const AppEntry& entry) const {
    if (entry.launchCount == 0) return 0.f;
    const qint64 now     = QDateTime::currentSecsSinceEpoch();
    const qint64 ageSecs = now - entry.lastUsed;
    const float  decay   = qExp(-float(ageSecs) / (7 * 86400.f)); // 7-day half-life
    return float(entry.launchCount) * decay;
}

// ─────────────────────────────────────────────────────────────────────────────
// Calculator
// ─────────────────────────────────────────────────────────────────────────────

QString AppLauncher::evalCalc(const QString& expr) const {
    // Very simple: support + - * / with integer/float operands
    // For a real implementation, use a proper expression parser
    // This is a placeholder that handles basic cases
    if (expr.isEmpty()) return {};

    QProcess proc;
    proc.start("python3", {"-c", QString("print(%1)").arg(expr)});
    if (proc.waitForFinished(500)) {
        const QString result = proc.readAllStandardOutput().trimmed();
        if (!result.isEmpty()) return result;
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::paintContent(QPainter& p) {  // header declares this override
    drawSearchBar(p);
    drawCategoryTabs(p);
    if (!m_calcResult.isEmpty())
        drawCalcResult(p);
    else if (m_mode == LauncherMode::Window)
        drawResults(p);  // window entries handled inside via m_openWindows
        else
            drawResults(p);
    drawScrollBar(p);
}

void AppLauncher::drawSearchBar(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const QRect r = searchBarRect();

    // Glass background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255,255,255, 18));
    p.drawRoundedRect(r, kSearchRadius, kSearchRadius);

    // Border
    p.setPen(QPen(QColor(255,255,255, 40), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, kSearchRadius, kSearchRadius);

    // Search icon
    p.setPen(theme.textMuted);
    p.setFont(searchFont());
    p.drawText(QRect(r.x() + 14, r.y(), 20, r.height()), Qt::AlignVCenter, "🔍");

    // Query text
    const QRect textR = r.adjusted(38, 0, -36, 0);
    p.setPen(m_query.isEmpty() ? theme.textMuted : theme.textPrimary);
    p.setFont(searchFont());
    const QFontMetrics fm(searchFont());
    const QString display = m_query.isEmpty() ? "Search applications…" : m_query;
    p.drawText(textR, Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(display, Qt::ElideRight, textR.width()));

    // Cursor blink
    if (!m_query.isEmpty() && m_cursorBlink > 0.5f) {
        const int curX = textR.x() + fm.horizontalAdvance(m_query.left(m_cursorPos));
        p.setPen(QPen(theme.accentColor, 1.5));
        p.drawLine(curX, r.y() + 10, curX, r.bottom() - 10);
    }

    // Clear button
    if (!m_query.isEmpty()) {
        const QRect cr = clearButtonRect();
        p.setPen(theme.textMuted);
        p.setFont(resultFont());
        p.drawText(cr, Qt::AlignCenter, "✕");
    }
}

void AppLauncher::drawCategoryTabs(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const QRect r = categoryTabRect();

    struct Tab { AppCategory cat; const char* label; };
    static const Tab kTabs[] = {
        { AppCategory::All,         "All"   },
        { AppCategory::Development, "Dev"   },
        { AppCategory::Internet,    "Net"   },
        { AppCategory::Multimedia,  "Media" },
        { AppCategory::Utility,     "Util"  },
        { AppCategory::System,      "Sys"   },
    };

    p.setFont(tabFont());
    int x = r.x();
    for (const auto& tab : kTabs) {
        const QFontMetrics fm(tabFont());
        const int w = fm.horizontalAdvance(tab.label) + 20;
        const QRect tr(x, r.y(), w, r.height());

        if (tab.cat == m_activeCategory) {
            p.setBrush(QColor(255,255,255, 25));
            p.setPen(QPen(theme.accentColor, 1));
            p.drawRoundedRect(tr.adjusted(1,2,-1,-2), 6, 6);
            p.setPen(theme.accentColor);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(Qt::NoPen);
            p.setPen(theme.textMuted);
        }
        p.drawText(tr, Qt::AlignCenter, tab.label);
        x += w + 4;
    }
}

void AppLauncher::drawResults(QPainter& p) {
    const int visible = visibleRowCount();
    const int total   = (m_mode == LauncherMode::Window)
    ? m_openWindows.size() : m_filtered.size();

    for (int i = 0; i < visible && (m_scrollOffset + i) < total; ++i) {
        const int absIdx = m_scrollOffset + i;
        if (m_mode == LauncherMode::Window) {
            drawWindowRow(p, i, m_openWindows[absIdx]);
        } else if (absIdx < m_filtered.size()) {
            drawResultRow(p, i, m_filtered[absIdx]);
        }
    }

    if (total == 0) drawEmptyState(p);
}

void AppLauncher::drawResultRow(QPainter& p, int index, const AppEntry& entry) {
    const auto& theme = Config::instance().theme;
    const QRect r = rowRect(index);
    const bool  sel = (index + m_scrollOffset == m_selectedIndex);
    const bool  hov = (index + m_scrollOffset == m_hoveredIndex);

    // Background
    if (sel) {
        QLinearGradient bg(r.topLeft(), r.bottomLeft());
        QColor c1 = theme.accentColor; c1.setAlpha(60);
        QColor c2 = theme.accentColor; c2.setAlpha(35);
        bg.setColorAt(0, c1); bg.setColorAt(1, c2);
        p.setBrush(bg);
        p.setPen(QPen(theme.accentColor, 1));
        p.drawRoundedRect(r, kRowRadius, kRowRadius);
    } else if (hov) {
        p.setBrush(QColor(255,255,255, 20));
        p.setPen(QPen(QColor(255,255,255, 30), 1));
        p.drawRoundedRect(r, kRowRadius, kRowRadius);
    }

    // Icon
    const QRect iconR(r.x() + kIconMargin, r.y() + (r.height() - kIconSize)/2,
                      kIconSize, kIconSize);
    const QPixmap px = iconForName(entry.iconName);
    if (!px.isNull())
        p.drawPixmap(iconR, px.scaled(kIconSize, kIconSize,
                                      Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else {
            p.setBrush(theme.accentColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(iconR, 6, 6);
            p.setPen(Qt::white);
            p.setFont(resultFont());
            p.drawText(iconR, Qt::AlignCenter,
                       entry.name.isEmpty() ? "?" : entry.name.left(1).toUpper());
        }

        // Name
        const int textX = iconR.right() + 10;
        const int textW = r.right() - textX - kPadH;
        QFont nf = resultFont();
        nf.setWeight(sel ? QFont::DemiBold : QFont::Normal);
        p.setFont(nf);
        p.setPen(sel ? Qt::white : theme.textPrimary);
        p.drawText(QRect(textX, r.y() + 8, textW, 20),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(nf).elidedText(entry.name, Qt::ElideRight, textW));

        // Subtitle
        if (!entry.description.isEmpty() && !entry.genericName.isEmpty()) {
            p.setFont(subFont());
            p.setPen(sel ? QColor(255,255,255,180) : theme.textMuted);
            p.drawText(QRect(textX, r.y() + 30, textW, 16),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(subFont()).elidedText(
                           entry.genericName.isEmpty() ? entry.description : entry.genericName,
                                                          Qt::ElideRight, textW));
        }
}

void AppLauncher::drawWindowRow(QPainter& p, int index, const WindowEntry& entry) {
    const auto& theme = Config::instance().theme;
    const QRect r = rowRect(index);
    const bool  sel = (index + m_scrollOffset == m_selectedIndex);

    if (sel) {
        p.setBrush(QColor(255,255,255, 25));
        p.setPen(QPen(theme.accentColor, 1));
        p.drawRoundedRect(r, kRowRadius, kRowRadius);
    }

    const QRect iconR(r.x() + kIconMargin, r.y() + (r.height() - kIconSize)/2,
                      kIconSize, kIconSize);
    if (!entry.icon.isNull())
        p.drawPixmap(iconR, entry.icon.pixmap(kIconSize, kIconSize));

    const int textX = iconR.right() + 10;
    p.setFont(resultFont());
    p.setPen(sel ? Qt::white : theme.textPrimary);
    p.drawText(QRect(textX, r.y() + 8, r.right() - textX - kPadH, 20),
               Qt::AlignLeft | Qt::AlignVCenter, entry.title);

    p.setFont(subFont());
    p.setPen(theme.textMuted);
    p.drawText(QRect(textX, r.y() + 30, r.right() - textX - kPadH, 16),
               Qt::AlignLeft | Qt::AlignVCenter,
               QString("Workspace %1").arg(entry.workspaceId));
}

void AppLauncher::drawEmptyState(QPainter& p) {
    const auto& theme = Config::instance().theme;
    p.setFont(resultFont());
    p.setPen(theme.textMuted);
    p.drawText(resultsRect(), Qt::AlignCenter,
               m_query.isEmpty() ? "No applications found" : "No results for "" + m_query + """);
}

void AppLauncher::drawCalcResult(QPainter& p) {
    const auto& theme = Config::instance().theme;
    const QRect r = resultsRect();

    p.setFont(searchFont());
    p.setPen(theme.textMuted);
    p.drawText(r.adjusted(0, 20, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
               "= " + m_calcResult);
}

void AppLauncher::drawScrollBar(QPainter& p) {
    const int total = m_filtered.size();
    const int vis   = visibleRowCount();
    if (total <= vis) return;

    const QRect rr = resultsRect();
    const int trackH = rr.height();
    const int thumbH = qMax(20, trackH * vis / total);
    const int thumbY = rr.y() + (trackH - thumbH) * m_scrollOffset / qMax(1, total - vis);

    p.setBrush(QColor(255,255,255, 30));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(rr.right() - kScrollBarW - 2, thumbY,
                      kScrollBarW, thumbH, 2, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Animation tick
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::onAnimTick() {
    if (!isVisible()) return;

    m_showProgress = qMin(m_showProgress + 0.08f, 1.0f);

    // Cursor blink (0.5 Hz)
    m_blinkPhase += kBlinkInterval;
    if (m_blinkPhase >= 1.0f) m_blinkPhase -= 1.0f;
    m_cursorBlink = m_blinkPhase < 0.5f ? 1.0f : 0.0f;

    update();
}

void AppLauncher::onIconsLoaded() {
    update();
}

void AppLauncher::onSearchChanged(const QString& text) {
    filterApps(text);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ─────────────────────────────────────────────────────────────────────────────

QRect AppLauncher::searchBarRect() const {
    return QRect(kPadH, kPadV, width() - 2 * kPadH, kSearchH);
}

QRect AppLauncher::categoryTabRect() const {
    const QRect s = searchBarRect();
    return QRect(kPadH, s.bottom() + 8, width() - 2 * kPadH, kTabH);
}

QRect AppLauncher::resultsRect() const {
    const QRect t = categoryTabRect();
    return QRect(kPadH, t.bottom() + 4,
                 width() - 2 * kPadH - kScrollBarW - 4,
                 height() - t.bottom() - 4 - kPadV);
}

QRect AppLauncher::rowRect(int index) const {
    const QRect rr = resultsRect();
    return QRect(rr.x(), rr.y() + index * kRowH, rr.width(), kRowH);
}

QRect AppLauncher::clearButtonRect() const {
    const QRect s = searchBarRect();
    return QRect(s.right() - 28, s.y(), 28, s.height());
}

int AppLauncher::visibleRowCount() const {
    return resultsRect().height() / kRowH;
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::selectNext() {
    const int total = (m_mode == LauncherMode::Window)
    ? m_openWindows.size() : m_filtered.size();
    selectIndex(qMin(m_selectedIndex + 1, total - 1));
}

void AppLauncher::selectPrev() {
    selectIndex(qMax(m_selectedIndex - 1, 0));
}

void AppLauncher::selectIndex(int index) {
    m_selectedIndex = index;
    // Scroll into view
    const int vis = visibleRowCount();
    if (m_selectedIndex < m_scrollOffset)
        m_scrollOffset = m_selectedIndex;
    else if (m_selectedIndex >= m_scrollOffset + vis)
        m_scrollOffset = m_selectedIndex - vis + 1;
    update();
}

void AppLauncher::confirmSelection() {
    if (m_mode == LauncherMode::Window) {
        if (m_selectedIndex < m_openWindows.size()) {
            emit windowRaised(m_openWindows[m_selectedIndex].id);
            close();
        }
        return;
    }

    if (!m_calcResult.isEmpty()) {
        // copy calc result to clipboard (stub)
        close();
        return;
    }

    if (m_selectedIndex >= m_filtered.size()) return;

    auto& entry = m_filtered[m_selectedIndex];
    QString cmd = entry.execClean;

    if (entry.terminal) {
        // Config has no terminal field — read from keybindings exec or use sensible default
        const QString term = Config::instance().keys.bindings.value(
            "terminal", "alacritty");
        cmd = term + " -e " + cmd;
    }

    recordLaunch(entry);
    emit appLaunched(cmd);
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Icon loading
// ─────────────────────────────────────────────────────────────────────────────

QPixmap AppLauncher::iconForName(const QString& iconName, int size) const {
    if (iconName.isEmpty()) return {};
    const QString key = iconName + "@" + QString::number(size);
    if (m_iconCache.contains(key)) return m_iconCache[key];

    QPixmap px;
    if (QFileInfo::exists(iconName)) {
        px = QPixmap(iconName).scaled(size, size,
                                      Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        const QIcon icon = QIcon::fromTheme(iconName);
        if (!icon.isNull()) px = icon.pixmap(size, size);
    }
    if (!px.isNull()) m_iconCache.insert(key, px);
    return px;
}

void AppLauncher::loadVisibleIcons() {
    const int vis = visibleRowCount();
    for (int i = 0; i < vis && (m_scrollOffset + i) < m_filtered.size(); ++i) {
        auto& entry = m_filtered[m_scrollOffset + i];
        if (entry.icon.isNull() && !entry.iconName.isEmpty())
            entry.icon = iconForName(entry.iconName);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Frecency
// ─────────────────────────────────────────────────────────────────────────────

QString AppLauncher::frecencyCachePath() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
    + "/applaunder_frecency.json";
}

void AppLauncher::loadFrecency() {
    QFile f(frecencyCachePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        m_launchCounts[it.key()] = entry["count"].toInt();
        m_lastUsedMap [it.key()] = entry["last"].toVariant().toLongLong();
    }
}

void AppLauncher::saveFrecency() const {
    QJsonObject root;
    for (auto it = m_launchCounts.begin(); it != m_launchCounts.end(); ++it) {
        QJsonObject e;
        e["count"] = it.value();
        e["last"]  = m_lastUsedMap.value(it.key(), 0);
        root[it.key()] = e;
    }
    QFile f(frecencyCachePath());
    QDir().mkpath(QFileInfo(f).absolutePath());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root).toJson());
}

void AppLauncher::recordLaunch(AppEntry& entry) {
    entry.launchCount++;
    entry.lastUsed = QDateTime::currentSecsSinceEpoch();
    m_launchCounts[entry.id] = entry.launchCount;
    m_lastUsedMap [entry.id] = entry.lastUsed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Keyboard / Mouse events
// ─────────────────────────────────────────────────────────────────────────────

void AppLauncher::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Escape:
            close(); return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            confirmSelection(); return;
        case Qt::Key_Down:
        case Qt::Key_Tab:
            selectNext(); return;
        case Qt::Key_Up:
            selectPrev(); return;
        case Qt::Key_Backspace:
            if (!m_query.isEmpty()) {
                m_query.chop(1);
                m_cursorPos = m_query.length();
                filterApps(m_query);
            }
            return;
        case Qt::Key_PageDown:
            selectIndex(qMin(m_selectedIndex + visibleRowCount(),
                             (int)m_filtered.size() - 1)); return;
        case Qt::Key_PageUp:
            selectIndex(qMax(m_selectedIndex - visibleRowCount(), 0)); return;
        default:
            break;
    }

    // Printable character → append to query
    const QString text = e->text();
    if (!text.isEmpty() && text[0].isPrint()) {
        m_query.insert(m_cursorPos, text);
        m_cursorPos += text.length();
        filterApps(m_query);
    }
}

void AppLauncher::mousePressEvent(QMouseEvent* e) {
    const QRect rr = resultsRect();
    if (rr.contains(e->pos())) {
        const int idx = m_scrollOffset + (e->pos().y() - rr.y()) / kRowH;
        selectIndex(idx);
        if (e->button() == Qt::LeftButton) confirmSelection();
        return;
    }
    if (clearButtonRect().contains(e->pos()) && !m_query.isEmpty()) {
        m_query.clear();
        m_cursorPos = 0;
        filterApps({});
    }
    // Category tab click
    const QRect tr = categoryTabRect();
    if (tr.contains(e->pos())) {
        struct Tab { AppCategory cat; int w; };
        const QStringList labels = {"All","Dev","Net","Media","Util","Sys"};
        const AppCategory cats[] = {
            AppCategory::All, AppCategory::Development, AppCategory::Internet,
            AppCategory::Multimedia, AppCategory::Utility, AppCategory::System
        };
        QFont f = tabFont();
        QFontMetrics fm(f);
        int x = tr.x();
        for (int i = 0; i < (int)std::size(cats); ++i) {
            const int w = fm.horizontalAdvance(labels[i]) + 20;
            if (QRect(x, tr.y(), w, tr.height()).contains(e->pos())) {
                m_activeCategory = cats[i];
                filterApps(m_query);
                break;
            }
            x += w + 4;
        }
    }
}

void AppLauncher::mouseMoveEvent(QMouseEvent* e) {
    const QRect rr = resultsRect();
    if (rr.contains(e->pos()))
        m_hoveredIndex = m_scrollOffset + (e->pos().y() - rr.y()) / kRowH;
    else
        m_hoveredIndex = -1;
    update();
}

void AppLauncher::wheelEvent(QWheelEvent* e) {
    const int delta = e->angleDelta().y() > 0 ? -1 : 1;
    const int total = m_filtered.size();
    const int vis   = visibleRowCount();
    m_scrollOffset  = qBound(0, m_scrollOffset + delta, qMax(0, total - vis));
    update();
}

void AppLauncher::focusOutEvent(QFocusEvent*) {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

QFont AppLauncher::searchFont() const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPixelSize(15);
    return f;
}

QFont AppLauncher::resultFont() const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPixelSize(13);
    return f;
}

QFont AppLauncher::subFont() const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPixelSize(11);
    return f;
}

QFont AppLauncher::tabFont() const {
    const auto& theme = Config::instance().theme;
    QFont f(theme.fontFamily);
    f.setPixelSize(12);
    return f;
}
