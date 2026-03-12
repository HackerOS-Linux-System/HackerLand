#include "LockScreen.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QtMath>
#include <QDebug>

LockScreen::LockScreen(QWidget* parent)
: QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);

    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(33);
    connect(m_pulseTimer, &QTimer::timeout, this, &LockScreen::onPulse);
}

LockScreen::~LockScreen() {}

void LockScreen::lock() {
    m_locked  = true;
    m_input.clear();
    m_failed  = false;
    m_shakeX  = 0.f;

    if (auto* scr = QGuiApplication::primaryScreen())
        setGeometry(scr->geometry());

    showFullScreen();
    raise();
    activateWindow();
    setFocus();
    m_pulseTimer->start();
    qInfo() << "[LockScreen] locked";
}

void LockScreen::unlock() {
    m_locked = false;
    m_pulseTimer->stop();
    hide();
    emit unlocked();
    qInfo() << "[LockScreen] unlocked";
}

void LockScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& theme = Config::instance().theme;
    const QRect r     = rect();
    const QPointF cx  = r.center();

    // ── Background: dark blurred glass ───────────────────────────────────
    p.fillRect(r, QColor(5, 8, 20, 240));

    // Radial glow in centre
    QRadialGradient glow(cx, r.height() * 0.4f);
    glow.setColorAt(0.0, QColor(theme.accentColor.red(),
                                theme.accentColor.green(),
                                theme.accentColor.blue(), 25));
    glow.setColorAt(1.0, Qt::transparent);
    p.fillRect(r, glow);

    // ── Clock display ──────────────────────────────────────────────────
    const QString timeStr = QTime::currentTime().toString("hh:mm");
    QFont clockFont(theme.fontFamily);
    clockFont.setPixelSize(96);
    clockFont.setWeight(QFont::Light);
    p.setFont(clockFont);
    p.setPen(QColor(240, 245, 255, 220));
    p.drawText(r.adjusted(0,0,0,-120), Qt::AlignHCenter | Qt::AlignVCenter, timeStr);

    // ── Password dots ─────────────────────────────────────────────────
    const int dotCount = qMin((int)m_input.size(), 16);
    const int dotR     = 8;
    const int dotGap   = 24;
    const float totalW = dotCount * (dotR*2 + dotGap) - dotGap;
    const float startX = cx.x() - totalW/2 + m_shakeX;
    const float dotY   = cx.y() + 80;

    for (int i = 0; i < dotCount; ++i) {
        const float x = startX + i * (dotR*2 + dotGap) + dotR;
        QColor dc = m_failed ? QColor(255, 80, 80)
        : theme.accentColor;
        dc.setAlpha(200);
        p.setPen(Qt::NoPen);
        p.setBrush(dc);
        p.drawEllipse(QPointF(x, dotY), dotR, dotR);
    }

    // ── Hint text ──────────────────────────────────────────────────────
    QFont hintFont(theme.fontFamily);
    hintFont.setPixelSize(14);
    p.setFont(hintFont);
    p.setPen(QColor(180, 190, 210, 120));
    const QString hint = m_failed ? "Wrong password — try again"
    : "Type password to unlock";
    p.drawText(r.adjusted(0, 140, 0, 0),
               Qt::AlignHCenter | Qt::AlignVCenter, hint);

    // ── Pulse ring ────────────────────────────────────────────────────
    const float ringR = 60.f + m_pulse * 8.f;
    QPainterPath ring;
    ring.addEllipse(cx, ringR, ringR);
    QPen rp(theme.accentColor, 1.5);
    rp.getColor()->setAlphaF(0.3f * (1.f - m_pulse));
    p.strokePath(ring, rp);
}

void LockScreen::keyPressEvent(QKeyEvent* e) {
    if (!m_locked) return;

    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        attemptUnlock();
    } else if (e->key() == Qt::Key_Backspace) {
        if (!m_input.isEmpty()) m_input.chop(1);
        m_failed = false;
        update();
    } else if (e->key() == Qt::Key_Escape) {
        m_input.clear();
        m_failed = false;
        update();
    } else if (!e->text().isEmpty() && e->text()[0].isPrint()) {
        m_input += e->text();
        update();
    }
}

void LockScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    setFocus();
}

void LockScreen::onPulse() {
    m_pulse += 0.03f;
    if (m_pulse > 1.f) m_pulse = 0.f;

    if (m_shakeTicks > 0) {
        --m_shakeTicks;
        m_shakeX = (m_shakeTicks % 4 < 2) ? 8.f : -8.f;
        if (m_shakeTicks == 0) m_shakeX = 0.f;
    }
    update();
}

void LockScreen::attemptUnlock() {
    // Use PAM via su -c "true" as simple auth check
    // (proper PAM needs libpam — this is a safe fallback)
    QProcess proc;
    proc.start("su", {"-c", "true", qgetenv("USER")});

    // Write password to stdin
    proc.waitForStarted(1000);
    proc.write((m_input + "\n").toLocal8Bit());
    proc.closeWriteChannel();
    proc.waitForFinished(3000);

    if (proc.exitCode() == 0) {
        unlock();
    } else {
        m_failed    = true;
        m_shakeTicks= kShakeDuration;
        m_input.clear();
        qInfo() << "[LockScreen] wrong password";
        update();
    }
}
