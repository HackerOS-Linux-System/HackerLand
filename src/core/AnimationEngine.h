#pragma once

#include <QObject>
#include <QRect>
#include <QHash>
#include <QList>
#include <QElapsedTimer>
#include <functional>
#include <memory>

class QVariantAnimation;
class Window;

// ─────────────────────────────────────────────────────────────────────────────
// AnimationType
//
// Semantic category of a running animation.  Used for priority decisions
// (e.g. a WindowClose animation cancels any in-flight WindowOpen) and for
// debug logging.
// ─────────────────────────────────────────────────────────────────────────────
enum class AnimationType {
    WindowOpen,        ///< Window appearing for the first time (scale-in + fade)
    WindowClose,       ///< Window being removed (scale-out + fade)
    WindowMove,        ///< Tiling rearrangement — geometry interpolation
    WindowResize,      ///< Interactive resize feedback (continuous, no easing)
    WorkspaceSwitch,   ///< Workspace slide-in / slide-out
    FadeIn,            ///< Generic opacity fade in
    FadeOut,           ///< Generic opacity fade out
    Custom             ///< Caller-driven animation via animateCustom()
};

// ─────────────────────────────────────────────────────────────────────────────
// EasingCurve
//
// Named easing functions available to callers.  The engine maps these to the
// actual mathematical functions in AnimationEngine::evaluate().
// ─────────────────────────────────────────────────────────────────────────────
enum class EasingCurve {
    Linear,         ///< f(t) = t
    EaseInCubic,    ///< f(t) = t³  — starts slow, accelerates
    EaseOutCubic,   ///< f(t) = 1-(1-t)³  — decelerates into target
    EaseInOutCubic, ///< Symmetric S-curve
    EaseOutBack,    ///< Overshoots slightly, then settles
    Spring,         ///< Damped oscillation — the HackerLand WM signature feel
    EaseOutQuint,   ///< Very fast deceleration (workspace switch)
    EaseInQuart     ///< Fast acceleration (window close)
};

// ─────────────────────────────────────────────────────────────────────────────
// WindowAnimation
//
// All state needed to drive a single per-window animation.
// Owned exclusively by AnimationEngine via the m_animations hash.
// ─────────────────────────────────────────────────────────────────────────────
struct WindowAnimation {
    // ── What is being animated ────────────────────────────────────────────
    AnimationType type    = AnimationType::WindowMove;

    // ── Geometry interpolation ────────────────────────────────────────────
    QRect  fromGeometry;
    QRect  toGeometry;

    // ── Opacity interpolation ─────────────────────────────────────────────
    float  fromOpacity    = 1.0f;
    float  toOpacity      = 1.0f;

    // ── Scale interpolation (for open / close) ────────────────────────────
    float  fromScale      = 1.0f;
    float  toScale        = 1.0f;

    // ── Timing ────────────────────────────────────────────────────────────
    int    durationMs     = 250;
    EasingCurve easing    = EasingCurve::EaseOutCubic;

    // ── Qt animation driver ───────────────────────────────────────────────
    std::unique_ptr<QVariantAnimation> anim;

    // ── Completion callback ───────────────────────────────────────────────
    std::function<void()> onFinished;

    WindowAnimation() = default;
    ~WindowAnimation() = default;

    // Non-copyable — unique_ptr member prevents it anyway.
    WindowAnimation(const WindowAnimation&)            = delete;
    WindowAnimation& operator=(const WindowAnimation&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// AnimationEngine
//
// Drives all window and workspace animations in HackerLand WM.
//
// Design
//   • One QVariantAnimation per animated window, stored in m_animations.
//   • Each animation runs a t ∈ [0,1] progress value through the configured
//     easing function, then applies the resulting interpolated geometry and
//     opacity to the Window model.
//   • WMOutput reads Window::geometry() and Window::opacity() every frame —
//     it does not need to know about animations at all.
//   • cancelAnimation() is always safe to call; it stops and deletes any
//     in-flight animation for the given window.
//   • A new animation on a window that is already animating cancels the
//     previous one first (no queuing).
//
// Spring physics
//   The signature "spring" easing used for WindowOpen and most tiling
//   rearrangements is a critically-under-damped oscillator:
//
//       f(t) = 1 − e^(−ζωt) · cos(ω√(1−ζ²) · t)
//
//   with ζ = 0.55 (damping ratio) and ω = 2π × 1.5 (natural frequency).
//   This produces a gentle overshoot followed by quick settle — the same
//   feel used by niri, Hyprland, and macOS Genie.
//
// Thread safety
//   All methods must be called on the compositor (main GUI) thread.
//   QVariantAnimation internally uses the Qt timer system and emits signals
//   on the thread it was started on.
// ─────────────────────────────────────────────────────────────────────────────
class AnimationEngine : public QObject {
    Q_OBJECT

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit AnimationEngine(QObject* parent = nullptr);
    ~AnimationEngine() override;

    // ── High-level animation launchers ───────────────────────────────────

    /// Animate a window appearing for the first time.
    /// Scales from Config::anim.scaleFactor → 1.0 and fades from 0 → 1
    /// using a spring easing over Config::anim.windowOpenMs milliseconds.
    void animateWindowOpen(Window* w, const QRect& targetGeom);

    /// Animate a window being removed.
    /// Scales toward its centre and fades out using an easeInCubic curve
    /// over Config::anim.windowCloseMs milliseconds.
    /// \p onDone is called once the animation completes (use it to call
    /// WMCompositor::removeWindowFromModel()).
    void animateWindowClose(Window* w,
                            std::function<void()> onDone = nullptr);

    /// Animate a window moving and/or resizing during a tiling rearrangement.
    /// Interpolates geometry from \p from → \p to using easeOutCubic over
    /// Config::anim.tileRearrangeMs milliseconds.
    void animateWindowMove(Window* w,
                           const QRect& from,
                           const QRect& to);

    /// Animate a workspace switch.
    /// All windows on the departing workspace slide out in \p direction
    /// while windows on the arriving workspace slide in from the opposite
    /// side over Config::anim.workspaceSwitchMs milliseconds.
    /// \p direction: "left", "right", "up", "down"
    void animateWorkspaceSwitch(const QList<Window*>& departing,
                                const QList<Window*>& arriving,
                                const QString&        direction,
                                const QRect&          outputRect);

    /// Animate opacity from \p from → \p to over \p ms milliseconds using
    /// the given easing curve.  Useful for focus / unfocus transitions.
    void animateOpacity(Window* w,
                        float   from,
                        float   to,
                        int     ms      = 150,
                        EasingCurve e   = EasingCurve::EaseOutCubic);

    /// Launch a fully custom animation on \p w.
    /// \p ticker is called each frame with the eased t ∈ [0, 1]; it is
    /// responsible for applying whatever properties it wants to the window.
    void animateCustom(Window*                     w,
                       int                         durationMs,
                       EasingCurve                 easing,
                       std::function<void(float t)> ticker,
                       std::function<void()>        onDone = nullptr);

    // ── Animation control ─────────────────────────────────────────────────

    /// Stop and discard any in-flight animation for \p w.
    /// The window is left in whatever intermediate state it was in;
    /// callers should snap it to the final geometry if needed.
    void cancelAnimation(Window* w);

    /// Cancel all in-flight animations (e.g. on workspace switch start).
    void cancelAll();

    /// True if \p w has an in-flight animation.
    bool isAnimating(Window* w) const;

    /// Number of currently active animations.
    int  activeCount() const { return m_animations.size(); }

    // ── Global enable / disable ───────────────────────────────────────────

    bool isEnabled()       const { return m_enabled; }
    void setEnabled(bool e)      { m_enabled = e; }

    // ── Easing evaluation (public for use by WMOutput render effects) ─────

    /// Evaluate easing function \p curve at normalised time \p t ∈ [0, 1].
    /// Returns a value in approximately [0, 1] (some curves overshoot slightly).
    static float evaluate(EasingCurve curve, float t);

    // ── String ↔ enum helpers ─────────────────────────────────────────────
    static EasingCurve easingFromString(const QString& s);
    static QString     easingToString(EasingCurve e);

signals:
    /// Emitted when a window's animation finishes naturally (not on cancel).
    void animationFinished(Window* w, AnimationType type);

    /// Emitted every time any animated window's properties change.
    /// WMOutput connects to this to trigger a repaint without polling.
    void frameReady();

private:
    // ── Internal animation builder ────────────────────────────────────────

    /// Construct and start a WindowAnimation for \p w.
    /// Any existing animation for \p w is cancelled first.
    WindowAnimation* startAnimation(Window*       w,
                                    AnimationType type,
                                    int           durationMs,
                                    EasingCurve   easing);

    // ── Per-frame tick (called by QVariantAnimation::valueChanged) ────────

    /// Apply interpolated geometry + opacity to \p w for raw progress \p raw.
    void applyFrame(Window* w, WindowAnimation* wa, float raw);

    // ── Easing implementations ────────────────────────────────────────────
    // These are the raw mathematical functions; prefer evaluate() from
    // outside the class.

    static float linear       (float t);
    static float easeInCubic  (float t);
    static float easeOutCubic (float t);
    static float easeInOutCubic(float t);
    static float easeOutBack  (float t);
    static float spring       (float t);
    static float easeOutQuint (float t);
    static float easeInQuart  (float t);

    // ── Geometry / opacity interpolation helpers ──────────────────────────

    /// Linear interpolation between two QRects.
    static QRect lerpRect(const QRect& a, const QRect& b, float t);

    /// Linear interpolation between two floats, clamped to [0, 1].
    static float lerpFloat(float a, float b, float t);

    // ── Members ───────────────────────────────────────────────────────────

    /// Active animations keyed by Window*.
    /// WindowAnimation is heap-allocated so its address is stable while
    /// lambdas captured in QVariantAnimation::valueChanged hold a raw pointer.
    QHash<Window*, WindowAnimation*> m_animations;

    bool m_enabled = true;

    // ── Spring physics constants ──────────────────────────────────────────
    static constexpr float kSpringZeta  = 0.55f;  ///< Damping ratio  ζ
    static constexpr float kSpringOmega = 9.42f;  ///< Natural freq   ω = 2π·1.5
    // kSpringOmega = 2 * M_PI * 1.5 ≈ 9.4248

    // ── easeOutBack overshoot constant ───────────────────────────────────
    static constexpr float kBackC1 = 1.70158f;
    static constexpr float kBackC3 = kBackC1 + 1.0f; // = 2.70158
};
