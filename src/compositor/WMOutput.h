#pragma once

#include <QOpenGLWidget>
#include <QScreen>
#include <QTimer>
#include <QPixmap>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QHash>
#include <QList>
#include <QString>
#include <QColor>
#include <QFont>
#include <QCursor>
#include <QElapsedTimer>
#include <QOpenGLFunctions_3_3_Core>
#include <QMovie>
#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions>
#include <memory>
#include <memory>

#include "WindowRenderState.h"

// Forward declarations
class WMCompositor;
class GLBlurRenderer;
class InputHandler;
class Window;

// ─────────────────────────────────────────────────────────────────────────────
// CursorShape
// ─────────────────────────────────────────────────────────────────────────────
enum class CursorShape {
    Normal,
    Resize,
    ResizeH,
    ResizeV,
    Move,
    Crosshair
};

// ─────────────────────────────────────────────────────────────────────────────
// ResizeEdge
// ─────────────────────────────────────────────────────────────────────────────
enum class ResizeEdge {
    None        = 0,
    Top         = 1 << 0,
    Bottom      = 1 << 1,
    Left        = 1 << 2,
    Right       = 1 << 3,
    TopLeft     = Top    | Left,
    TopRight    = Top    | Right,
    BottomLeft  = Bottom | Left,
    BottomRight = Bottom | Right
};
Q_DECLARE_FLAGS(ResizeEdges, ResizeEdge)
Q_DECLARE_OPERATORS_FOR_FLAGS(ResizeEdges)

// ─────────────────────────────────────────────────────────────────────────────
// WMOutput
// ─────────────────────────────────────────────────────────────────────────────
class WMOutput : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit WMOutput(WMCompositor* compositor,
                      QScreen*      screen,
                      QObject*      parent = nullptr);
    ~WMOutput() override;

    QScreen* screen() const { return m_screen; }

    void show();
    void hide();

    bool    isDragging()    const { return m_dragging; }
    bool    isResizing()    const { return m_resizing; }
    Window* hoveredWindow() const { return m_hoveredWindow; }

signals:
    void actionTriggered(const QString& action);

protected:
    void initializeGL()                   override;
    void resizeGL(int w, int h)           override;
    void paintGL()                        override;
    void paintEvent(QPaintEvent* event)   override;
    void resizeEvent(QResizeEvent* event) override;

    void keyPressEvent    (QKeyEvent*    event) override;
    void keyReleaseEvent  (QKeyEvent*    event) override;
    void mousePressEvent  (QMouseEvent*  event) override;
    void mouseReleaseEvent(QMouseEvent*  event) override;
    void mouseMoveEvent   (QMouseEvent*  event) override;
    void wheelEvent       (QWheelEvent*  event) override;
    void enterEvent       (QEnterEvent*  event) override;
    void leaveEvent       (QEvent*       event) override;

private slots:
    void onRenderTick();   // ← slot declaration (was missing)

private:
    // ── Draw passes ───────────────────────────────────────────────────────
    void initAnimShader         ();
    void drawWallpaper          (QPainter& p);
    void drawVignette           (QPainter& p);
    void drawWindows            (QPainter& p);
    void drawWindow             (QPainter& p, Window* w, bool isActive);
    void drawWindowShadow       (QPainter& p, const QRect& rect, bool active);
    void drawWindowBlurBackground(QPainter& p, Window* w, const QRect& rect);
    void drawWindowGlassOverlay (QPainter& p, const QRect& rect, bool active);
    void drawWindowBorder       (QPainter& p, const QRect& rect, bool active);
    void drawTitleBar           (QPainter& p, Window* w, bool active);
    void drawTitleBarSeparator  (QPainter& p, const QRect& windowRect, bool active);
    void drawWindowContent      (QPainter& p, Window* w);   // ← was missing
    void drawCursor             (QPainter& p);

    // ── Wallpaper helpers ─────────────────────────────────────────────────
    void    loadWallpaper();
    void    rescaleWallpaper();
    QPixmap generateFallbackWallpaper(const QSize& size) const;

    // ── Blur helpers ──────────────────────────────────────────────────────
    QImage        blurWallpaperRegion(Window* w, const QRect& windowRect);
    static QImage boxBlur(const QImage& src, int radius);

    // ── Input helpers ─────────────────────────────────────────────────────
    void       dispatchKeybind(QKeyEvent* e);
    void       handleAction(const QString& action);
    Window*    windowAt(const QPoint& pos) const;
    ResizeEdge resizeEdgeAt(const QPoint& pos, Window* w) const;
    void       updateCursorShape();

    // ── Traffic-light hit-testing ─────────────────────────────────────────
    enum class TitleBarButton { None, Close, Maximize, Minimize };
    TitleBarButton titleBarButtonAt(const QPoint& pos, Window* w) const;
    QRect closeButtonRect   (const QRect& windowRect) const;
    QRect maximizeButtonRect(const QRect& windowRect) const;
    QRect minimizeButtonRect(const QRect& windowRect) const;

    // ── Render state cache ────────────────────────────────────────────────
    WindowRenderState& renderStateFor(Window* w);
    void invalidateAllBlurCaches();

    // ── Members ───────────────────────────────────────────────────────────
    WMCompositor* m_compositor    = nullptr;
    QScreen*      m_screen        = nullptr;

    QTimer*       m_renderTimer   = nullptr;
    QElapsedTimer m_frameTimer;
    qint64        m_lastFrameMs   = 0;

    QPixmap       m_wallpaper;
    QPixmap       m_wallpaperScaled;
    bool          m_wallpaperDirty = true;

    // ← was missing (used in resizeEvent)
    QPixmap       m_blurCache;
    bool          m_needsRedraw   = true;

    // OpenGL functions (3.3 core profile)
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
    bool                m_glAvailable = false;

    // GL blur renderer (GPU)
    GLBlurRenderer*     m_glBlur      = nullptr;

    // Animated wallpaper shader
    QOpenGLShaderProgram* m_animShader = nullptr;
    GLuint              m_animVao     = 0;
    GLuint              m_animVbo     = 0;
    float               m_animTime    = 0.f;

    // Animated GIF wallpaper
    QMovie*             m_wallpaperMovie = nullptr;

    // InputHandler (created by WMCompositor, passed here)
    InputHandler*       m_inputHandler   = nullptr;

    QHash<Window*, WindowRenderState> m_renderStates;

    float         m_glowPulse     = 0.0f;
    float         m_glowDir       = 1.0f;

    QPoint        m_cursorPos;
    CursorShape   m_cursorShape   = CursorShape::Normal;
    Window*       m_hoveredWindow = nullptr;

    bool          m_dragging      = false;
    Window*       m_dragWindow    = nullptr;
    QPoint        m_dragOffset;
    QRect         m_dragStartGeom;

    bool          m_resizing      = false;
    Window*       m_resizeWindow  = nullptr;
    ResizeEdge    m_resizeEdge    = ResizeEdge::None;
    QPoint        m_resizeStartCursor;
    QRect         m_resizeStartGeom;

    Qt::KeyboardModifiers m_currentModifiers = Qt::NoModifier;

    static constexpr int   kTitleBarHeight    = 28;
    static constexpr int   kResizeHandleWidth =  6;
    static constexpr int   kDotRadius         =  5;
    static constexpr int   kDotSpacing        = 16;
    static constexpr int   kDotRightMargin    = 12;
    static constexpr float kBlurRadius        = 14.0f;
    static constexpr float kGlowPulseSpeed    = 0.012f;
};
