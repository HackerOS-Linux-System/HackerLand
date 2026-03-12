// ─────────────────────────────────────────────────────────────────────────────
// LockScreen.cpp — HackerLand WM
//
// Swaylock-style fullscreen lock overlay.
// Wywołanie: Super+L, IPC "lock", lub WMCompositor::lockScreen()
//
// Autentykacja:
//   1. Próbuje PAM przez /sbin/unix_chkpwd (setuid, działa bez root)
//   2. Fallback: su -c "true" <user>
//   3. Dev fallback: hasło "test" (tylko w trybie debug)
//
// Wygląd:
//   • Ciemne, rozmyte tło (gradient + mgławice)
//   • Duży zegar hh:mm na środku
//   • Kropki hasła (max 32, maskowane)
//   • Animacja pulsowania pierścienia
//   • Shake + czerwone kropki przy złym haśle
//   • Tekst podpowiedzi
// ─────────────────────────────────────────────────────────────────────────────

#include "LockScreen.h"
#include "core/Config.h"

#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QDateTime>
#include <QTime>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QtMath>
#include <QDebug>

#include <unistd.h>
#include <cstring>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Konstruktor
// ─────────────────────────────────────────────────────────────────────────────

LockScreen::LockScreen(QWidget* parent)
: QWidget(parent,
          Qt::Window |
          Qt::FramelessWindowHint |
          Qt::WindowStaysOnTopHint)
{
    // Nieprzezroczyste, pełnoekranowe
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent,      true);
    setAttribute(Qt::WA_DeleteOnClose,         false);

    // Klawiatura wyłącznie do nas — żadne shortcuty WM nie przechodzą
    setFocusPolicy(Qt::StrongFocus);
    grabKeyboard();

    // Timer animacji: ~30 fps
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(33);
    connect(m_pulseTimer, &QTimer::timeout,
            this, &LockScreen::onPulse);

    // Timer zegara: odświeżaj co sekundę
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout,
            this, [this]{ update(); });
}

LockScreen::~LockScreen() {
    releaseKeyboard();
}

// ─────────────────────────────────────────────────────────────────────────────
// lock() / unlock()
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::lock() {
    m_locked      = true;
    m_input.clear();
    m_failed      = false;
    m_shakeX      = 0.f;
    m_shakeTicks  = 0;
    m_pulse       = 0.f;
    m_authBusy    = false;

    // Rozciągnij na cały ekran główny
    if (QScreen* scr = QGuiApplication::primaryScreen())
        setGeometry(scr->geometry());

    showFullScreen();
    raise();
    activateWindow();
    setFocus();
    grabKeyboard();

    m_pulseTimer->start();
    m_clockTimer->start();

    qInfo() << "[LockScreen] locked";
}

void LockScreen::unlock() {
    m_locked = false;
    m_pulseTimer->stop();
    m_clockTimer->stop();
    releaseKeyboard();
    hide();
    emit unlocked();
    qInfo() << "[LockScreen] unlocked";
}

// ─────────────────────────────────────────────────────────────────────────────
// paintEvent — cały wygląd
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto&   theme = Config::instance().theme;
    const QRect   r     = rect();
    const QPointF cx    = QPointF(r.center());
    const float   w     = float(r.width());
    const float   h     = float(r.height());

    // ── Tło: ciemny kosmos ────────────────────────────────────────────────
    {
        QLinearGradient bg(0, 0, w, h);
        bg.setColorAt(0.0, QColor(3,  5,  15));
        bg.setColorAt(0.5, QColor(6,  10, 25));
        bg.setColorAt(1.0, QColor(4,  7,  18));
        p.fillRect(r, bg);
    }

    // Mgławice
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    struct Neb { float x,y,rad; QColor c; };
    const Neb nebulae[] = {
        { 0.20f, 0.25f, 0.30f, QColor(30,  60, 160, 40) },
        { 0.75f, 0.60f, 0.25f, QColor(70,  25, 140, 35) },
        { 0.50f, 0.80f, 0.20f, QColor(15, 100, 120, 30) },
    };
    for (const auto& n : nebulae) {
        QRadialGradient ng(n.x*w, n.y*h, n.rad*qMin(w,h));
        ng.setColorAt(0, n.c); ng.setColorAt(1, Qt::transparent);
        p.fillRect(r, ng);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Vignette
    {
        QRadialGradient vig(cx, qMax(w,h)*0.7f);
        vig.setColorAt(0.5, Qt::transparent);
        vig.setColorAt(1.0, QColor(0,0,0,120));
        p.fillRect(r, vig);
    }

    // ── Zegar ─────────────────────────────────────────────────────────────
    {
        const QString timeStr = QTime::currentTime().toString("hh:mm");
        const QString dateStr = QDate::currentDate().toString("dddd, d MMMM yyyy");

        QFont clockFont(theme.fontFamily);
        clockFont.setPixelSize(int(h * 0.14f));
        clockFont.setWeight(QFont::Thin);
        p.setFont(clockFont);
        p.setPen(QColor(230, 240, 255, 210));

        // Cień zegara
        p.setPen(QColor(0,0,0,80));
        QRect clockRect = r.adjusted(0, 0, 0, int(-h*0.25f));
        p.drawText(clockRect.adjusted(2,3,2,3), Qt::AlignHCenter|Qt::AlignVCenter, timeStr);

        p.setPen(QColor(230, 240, 255, 210));
        p.drawText(clockRect, Qt::AlignHCenter|Qt::AlignVCenter, timeStr);

        // Data pod zegarem
        QFont dateFont(theme.fontFamily);
        dateFont.setPixelSize(int(h * 0.022f));
        dateFont.setWeight(QFont::Light);
        p.setFont(dateFont);
        p.setPen(QColor(160, 175, 200, 160));
        QRect dateRect(0, int(cx.y() - h*0.06f),
                       r.width(), int(h*0.04f));
        p.drawText(dateRect, Qt::AlignHCenter|Qt::AlignVCenter, dateStr);
    }

    // ── Pierścień pulsowania ──────────────────────────────────────────────
    {
        const float  ringBaseR = qMin(w,h) * 0.09f;
        const float  ringR     = ringBaseR + m_pulse * ringBaseR * 0.15f;
        const QPointF dotCenter(cx.x(), cx.y() + h * 0.16f);

        // Zewnętrzny pulsujący pierścień
        QPainterPath ring;
        ring.addEllipse(dotCenter, ringR, ringR);
        QColor ringColor = m_failed ? QColor(220, 50, 50)
        : theme.accentColor;
        ringColor.setAlphaF(0.18f * (1.f - m_pulse * 0.5f));
        p.fillPath(ring, ringColor);

        // Wewnętrzny pierścień (stały)
        QPainterPath innerRing;
        innerRing.addEllipse(dotCenter, ringBaseR * 0.92f, ringBaseR * 0.92f);
        QPainterPath innerFill;
        innerFill.addEllipse(dotCenter, ringBaseR * 0.80f, ringBaseR * 0.80f);
        QPainterPath borderOnly = innerRing.subtracted(innerFill);

        QColor borderColor = m_failed ? QColor(220, 60, 60, 140)
        : QColor(theme.accentColor.red(),
                 theme.accentColor.green(),
                 theme.accentColor.blue(), 100);
        p.fillPath(borderOnly, borderColor);

        // ── Kropki hasła ──────────────────────────────────────────────────
        const int   dotCount  = qMin((int)m_input.size(), kMaxDots);
        const int   dotR      = int(ringBaseR * 0.18f);
        const int   dotGap    = int(ringBaseR * 0.50f);
        const float totalW    = dotCount > 0
        ? float(dotCount) * (dotR*2 + dotGap) - dotGap
        : 0.f;
        const float startX    = float(dotCenter.x()) - totalW/2.f + m_shakeX;
        const float dotsY     = float(dotCenter.y());

        for (int i = 0; i < dotCount; ++i) {
            const float x = startX + float(i) * float(dotR*2 + dotGap) + dotR;
            QColor dc = m_failed ? QColor(220, 70, 70, 220)
            : QColor(theme.accentColor.red(),
                     theme.accentColor.green(),
                     theme.accentColor.blue(), 220);
            // Gradient na każdej kropce
            QRadialGradient dg(x, dotsY - dotR*0.3f, dotR*1.2f);
            dg.setColorAt(0.0, dc.lighter(130));
            dg.setColorAt(1.0, dc);
            p.setPen(Qt::NoPen);
            p.setBrush(QBrush(dg));
            p.drawEllipse(QPointF(x, dotsY), float(dotR), float(dotR));
        }

        // Ikona kłódki gdy brak wpisanych znaków
        if (dotCount == 0) {
            QFont iconFont(theme.fontFamily);
            iconFont.setPixelSize(int(ringBaseR * 0.65f));
            p.setFont(iconFont);
            p.setPen(QColor(160, 175, 200, 100));
            p.drawText(QRectF(dotCenter.x() - ringBaseR,
                              dotCenter.y() - ringBaseR,
                              ringBaseR*2, ringBaseR*2),
                       Qt::AlignHCenter|Qt::AlignVCenter, "🔒");
        }
    }

    // ── Tekst podpowiedzi ─────────────────────────────────────────────────
    {
        const QString hint = m_authBusy  ? "Weryfikowanie..."
        : m_failed    ? "Złe hasło — spróbuj ponownie"
        : "Wpisz hasło aby odblokować";

        QFont hintFont(theme.fontFamily);
        hintFont.setPixelSize(int(h * 0.018f));
        p.setFont(hintFont);

        QColor hintColor = m_failed ? QColor(220, 100, 100, 200)
        : QColor(150, 165, 195, 150);
        p.setPen(hintColor);

        QRect hintRect(0, int(cx.y() + h * 0.26f),
                       r.width(), int(h * 0.05f));
        p.drawText(hintRect, Qt::AlignHCenter|Qt::AlignVCenter, hint);
    }

    // ── Autor / hostname ──────────────────────────────────────────────────
    {
        const QString user = qgetenv("USER");
        QFont smallFont(theme.fontFamily);
        smallFont.setPixelSize(int(h * 0.014f));
        p.setFont(smallFont);
        p.setPen(QColor(100, 115, 145, 100));
        p.drawText(r.adjusted(0, 0, 0, -int(h*0.025f)),
                   Qt::AlignHCenter|Qt::AlignBottom,
                   user.isEmpty() ? "" : ("🖥  " + user));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// keyPressEvent — obsługa klawiatury
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::keyPressEvent(QKeyEvent* e) {
    if (!m_locked || m_authBusy) return;

    switch (e->key()) {

        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!m_input.isEmpty())
                attemptUnlock();
        break;

        case Qt::Key_Backspace:
            if (!m_input.isEmpty()) {
                m_input.chop(1);
                m_failed = false;
                update();
            }
            break;

        case Qt::Key_Escape:
            // Czyść hasło, ale nie odblokowuj
            m_input.clear();
            m_failed = false;
            update();
            break;

        default:
            // Przyjmuj tylko drukowalne znaki (nie kombinacje Ctrl/Alt)
            if (!e->text().isEmpty() &&
                e->text()[0].isPrint() &&
                !(e->modifiers() & Qt::ControlModifier) &&
                !(e->modifiers() & Qt::AltModifier) &&
                !(e->modifiers() & Qt::MetaModifier))
            {
                if ((int)m_input.size() < kMaxPassword)
                    m_input += e->text();
                m_failed = false;
                update();
            }
            break;
    }

    // Blokuj wszystkie eventy — żadne skróty WM nie przechodzą przez LockScreen
    e->accept();
}

// ─────────────────────────────────────────────────────────────────────────────
// showEvent
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    setFocus();
    grabKeyboard();
}

// ─────────────────────────────────────────────────────────────────────────────
// onPulse — animacja
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::onPulse() {
    // Pulsowanie pierścienia
    m_pulse += 0.025f;
    if (m_pulse > 1.f) m_pulse = 0.f;

    // Shake przy złym haśle
    if (m_shakeTicks > 0) {
        --m_shakeTicks;
        const float phase = float(kShakeDuration - m_shakeTicks);
        m_shakeX = std::sin(phase * 1.4f) * 10.f
        * (float(m_shakeTicks) / float(kShakeDuration));
        if (m_shakeTicks == 0) m_shakeX = 0.f;
    }

    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// attemptUnlock — autentykacja
// ─────────────────────────────────────────────────────────────────────────────

void LockScreen::attemptUnlock() {
    if (m_authBusy || m_input.isEmpty()) return;
    m_authBusy = true;
    update(); // pokaż "Weryfikowanie..."

    const QString password = m_input;
    m_input.clear();

    // ── Metoda 1: unix_chkpwd (setuid, standardowy PAM helper) ───────────
    // Działa bez root na każdym systemie z PAM
    bool authenticated = false;

    {
        QProcess proc;
        proc.setProgram("/sbin/unix_chkpwd");
        proc.setArguments({"hackerlandwm", "chkexpiry"});
        proc.start();
        if (proc.waitForStarted(1000)) {
            proc.write((password + "\n").toLocal8Bit());
            proc.closeWriteChannel();
            proc.waitForFinished(5000);
            authenticated = (proc.exitCode() == 0);
        }
    }

    // ── Metoda 2: fallback przez su ───────────────────────────────────────
    if (!authenticated) {
        QProcess proc;
        const QString user = qgetenv("USER");
        proc.setProgram("su");
        proc.setArguments({"-c", "true", "--", user});
        proc.start();
        if (proc.waitForStarted(1000)) {
            proc.write((password + "\n").toLocal8Bit());
            proc.closeWriteChannel();
            proc.waitForFinished(5000);
            authenticated = (proc.exitCode() == 0);
        }
    }

    // ── Metoda 3: dev fallback (tylko debug) ─────────────────────────────
    #ifdef HACKERLANDWM_DEV_LOCK
    if (!authenticated && password == "test") {
        qDebug() << "[LockScreen] DEV fallback: hasło 'test' zaakceptowane";
        authenticated = true;
    }
    #endif

    m_authBusy = false;

    if (authenticated) {
        qInfo() << "[LockScreen] autentykacja OK";
        unlock();
    } else {
        qInfo() << "[LockScreen] złe hasło";
        m_failed     = true;
        m_shakeTicks = kShakeDuration;
        m_shakeX     = 0.f;
        update();
    }
}
