#pragma once

#include <QObject>
#include <QString>
#include <QSize>
#include <QRect>
#include <QTimer>
#include <QImage>
#include <QProcess>
#include <functional>

class WMCompositor;
class WMOutput;

// ─────────────────────────────────────────────────────────────────────────────
// ScreencastSession — jedna sesja nagrywania / udostępniania ekranu
// ─────────────────────────────────────────────────────────────────────────────
struct ScreencastSession {
    uint32_t    nodeId   = 0;       // PipeWire node ID
    QString     path;               // DBus object path
    bool        active   = false;
    QSize       size;
    int         fps      = 30;
};

// ─────────────────────────────────────────────────────────────────────────────
// ScreencastManager
//
// Implementuje xdg-desktop-portal screencasting przez PipeWire.
// Obsługuje:
//   • Udostępnianie ekranu (screen share) — np. dla przeglądarek, OBS
//   • Nagrywanie do pliku MP4/MKV przez ffmpeg (opcjonalne)
//   • Zrzut ekranu (screenshot) do PNG/JPG
//
// Architektura:
//   1. DBus: nasłuchuje na org.freedesktop.portal.ScreenCast
//   2. PipeWire: tworzy węzeł video/source i pushuje klatki
//   3. Klatki: grabowane z QOpenGLWidget::grabFramebuffer()
//
// Kompilacja:
//   Wymaga libpipewire-0.3-dev — jeśli nieobecne, cała klasa to stub.
//   CMakeLists.txt automatycznie wykrywa i dodaje -DHAVE_PIPEWIRE.
// ─────────────────────────────────────────────────────────────────────────────
class ScreencastManager : public QObject {
    Q_OBJECT

public:
    explicit ScreencastManager(WMCompositor* compositor,
                               QObject*      parent = nullptr);
    ~ScreencastManager() override;

    // ── Inicjalizacja ──────────────────────────────────────────────────────
    bool initialize();
    void shutdown();
    bool isAvailable() const;   // true jeśli PipeWire działa

    // ── Zrzut ekranu ──────────────────────────────────────────────────────
    // Zapisuje PNG/JPG do podanej ścieżki.
    // Jeśli path jest pusty — zapisuje do ~/Zrzuty ekranu/screenshot-TIMESTAMP.png
    // Zwraca ścieżkę do pliku lub "" przy błędzie.
    QString takeScreenshot(const QString& path     = {},
                           const QRect&   region   = {},
                           const QString& format   = "png");

    // ── Nagrywanie do pliku ────────────────────────────────────────────────
    bool startRecording(const QString& outputPath = {},
                        int            fps        = 30,
                        const QRect&   region     = {});
    void stopRecording();
    bool isRecording() const { return m_recording; }
    QString recordingPath() const { return m_recordingPath; }

    // ── PipeWire stream (dla screen share) ────────────────────────────────
    bool  startStream(int fps = 30);
    void  stopStream();
    bool  isStreaming()  const { return m_streaming; }
    uint32_t pipeWireNodeId() const;

signals:
    void screenshotSaved  (const QString& path);
    void screenshotFailed (const QString& reason);
    void recordingStarted (const QString& path);
    void recordingStopped (const QString& path);
    void streamStarted    (uint32_t nodeId);
    void streamStopped    ();
    void frameReady       (const QImage& frame);  // dla podglądu

private slots:
    void onCaptureTick();

private:
    // ── Frame capture ──────────────────────────────────────────────────────
    QImage captureFrame(const QRect& region = {});

    // ── PipeWire helpers ───────────────────────────────────────────────────
    bool  initPipeWire();
    void  cleanupPipeWire();
    void  pushFrameToPipeWire(const QImage& frame);

    // ── FFmpeg helpers (nagrywanie) ────────────────────────────────────────
    bool  startFFmpeg(const QString& outPath, const QSize& size, int fps);
    void  stopFFmpeg();
    void  sendFrameToFFmpeg(const QImage& frame);

    // ── Screenshot helpers ─────────────────────────────────────────────────
    static QString defaultScreenshotDir();
    static QString timestampedFilename(const QString& ext);

    // ── Members ───────────────────────────────────────────────────────────
    WMCompositor* m_compositor  = nullptr;

    // Capture timer
    QTimer*       m_captureTimer= nullptr;
    QRect         m_captureRegion;
    int           m_captureFps  = 30;

    // Recording state
    bool          m_recording     = false;
    QString       m_recordingPath;
    QProcess*     m_ffmpegProc    = nullptr;
    int           m_frameCount    = 0;

    // PipeWire stream state
    bool          m_streaming     = false;
    bool          m_pwAvailable   = false;

    // PipeWire opaque pointers (defined only when HAVE_PIPEWIRE)
    struct PWState;
    PWState*      m_pw            = nullptr;
};
