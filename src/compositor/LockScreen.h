#pragma once
#include <QWidget>
#include <QTimer>
#include <QDate>
#include <QLineEdit>
#include <QString>

// ─────────────────────────────────────────────────────────────────────────────
// LockScreen — swaylock-style fullscreen lock overlay
// Triggered by: Super+L keybind, idle timeout, or IPC "lock" command
// Authentication: PAM via /etc/pam.d/hackerlandwm (falls back to /bin/su)
// ─────────────────────────────────────────────────────────────────────────────
class LockScreen : public QWidget {
    Q_OBJECT
public:
    explicit LockScreen(QWidget* parent = nullptr);
    ~LockScreen() override;

    void lock();
    void unlock();
    bool isLocked() const { return m_locked; }

signals:
    void unlocked();

protected:
    void paintEvent   (QPaintEvent*  e) override;
    void keyPressEvent(QKeyEvent*    e) override;
    void showEvent    (QShowEvent*   e) override;

private slots:
    void onPulse();
    void attemptUnlock();

private:
    bool    m_locked    = false;
    QString m_input;
    bool    m_failed    = false;
    float   m_pulse     = 0.f;
    float   m_shakeX    = 0.f;
    QTimer* m_pulseTimer= nullptr;

    // Shake animation on wrong password
    int     m_shakeTicks = 0;
    bool    m_authBusy   = false;
    QTimer* m_clockTimer = nullptr;

    static constexpr int kShakeDuration = 20;
    static constexpr int kMaxDots       = 32;
    static constexpr int kMaxPassword   = 256;
};
