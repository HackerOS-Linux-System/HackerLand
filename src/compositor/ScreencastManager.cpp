// ─────────────────────────────────────────────────────────────────────────────
// ScreencastManager.cpp — HackerLand WM
//
// Screencasting i nagrywanie ekranu przez PipeWire + ffmpeg.
//
// Jak to działa:
//   1. captureFrame() — grabuje aktualną klatkę z QOpenGLWidget (GPU→CPU)
//   2. pushFrameToPipeWire() — wysyła do PipeWire (screen share dla np. Firefox)
//   3. sendFrameToFFmpeg()   — pipe do ffmpeg stdin (nagrywanie pliku)
//   4. takeScreenshot()      — jednorazowy grab → QImage → plik PNG/JPG
//
// PipeWire:
//   Kod kompiluje się zarówno z PipeWire jak i bez niego.
//   Gdy libpipewire nie jest dostępne (brak pkg), cała funkcja stream
//   jest zastąpiona stubem który loguje ostrzeżenie.
//   Streamy PipeWire wymagają xdg-desktop-portal do rejestracji node,
//   ale podstawowe działanie (ffmpeg nagrywanie + screenshot) działa
//   całkowicie bez portalu.
//
// Nagrywanie:
//   Uruchamia ffmpeg jako subprocess, przesyła klatki RGBA przez stdin pipe.
//   Nie wymaga żadnych dodatkowych bibliotek.
//   Format wyjściowy: MP4 (H.264) lub MKV.
//
// Wymagania systemowe:
//   • libpipewire-0.3-dev  (opcjonalne — do screen share)
//   • ffmpeg w PATH        (opcjonalne — do nagrywania)
//   • Zrzut ekranu działa zawsze bez żadnych zależności
// ─────────────────────────────────────────────────────────────────────────────

#include "ScreencastManager.h"
#include "WMCompositor.h"
#include "WMOutput.h"

#include <QPainter>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QScreen>
#include <QOpenGLWidget>
#include <QTimer>
#include <QDebug>
#include <QImageWriter>
#include <QBuffer>

// PipeWire headers — opcjonalne
#ifdef HAVE_PIPEWIRE
#  include <pipewire/pipewire.h>
#  include <pipewire/stream.h>
#  include <spa/param/video/format-utils.h>
#  include <spa/param/props.h>
#  include <spa/utils/result.h>
#endif

#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// PWState — wewnętrzne dane PipeWire (zdefiniowane tylko gdy HAVE_PIPEWIRE)
// ─────────────────────────────────────────────────────────────────────────────
struct ScreencastManager::PWState {
    #ifdef HAVE_PIPEWIRE
    pw_thread_loop* loop    = nullptr;
    pw_context*     context = nullptr;
    pw_core*        core    = nullptr;
    pw_stream*      stream  = nullptr;
    uint32_t        nodeId  = 0;
    bool            connected = false;

    // Rozmiar ramki
    int             width   = 0;
    int             height  = 0;

    // Wskaźnik do managera (do callbacków)
    ScreencastManager* mgr  = nullptr;
    #else
    int placeholder = 0; // struct nie może być pusta w C++
    #endif
};

// ─────────────────────────────────────────────────────────────────────────────
// PipeWire callbacks (tylko gdy HAVE_PIPEWIRE)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAVE_PIPEWIRE

static void pw_stream_state_changed(void* data,
                                    pw_stream_state old_state,
                                    pw_stream_state new_state,
                                    const char* error)
{
    Q_UNUSED(old_state);
    auto* st = static_cast<ScreencastManager::PWState*>(data);
    qInfo() << "[PipeWire] stream state:"
    << pw_stream_state_as_string(new_state);

    if (error) qWarning() << "[PipeWire] error:" << error;

    if (new_state == PW_STREAM_STATE_STREAMING) {
        st->connected = true;
        if (st->mgr)
            QMetaObject::invokeMethod(st->mgr, [st]{
                emit st->mgr->streamStarted(st->nodeId);
            }, Qt::QueuedConnection);
    } else if (new_state == PW_STREAM_STATE_ERROR ||
        new_state == PW_STREAM_STATE_UNCONNECTED) {
        st->connected = false;
        }
}

static void pw_stream_process(void* data) {
    // Wywoływane gdy konsument (np. Firefox) chce następną klatkę.
    // Implementacja: nie pushujemy tutaj — klatki są pushowane przez timer.
    Q_UNUSED(data);
}

static const pw_stream_events kStreamEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = pw_stream_state_changed,
    .process       = pw_stream_process,
};

#endif // HAVE_PIPEWIRE

// ─────────────────────────────────────────────────────────────────────────────
// Konstruktor / destruktor
// ─────────────────────────────────────────────────────────────────────────────

ScreencastManager::ScreencastManager(WMCompositor* compositor, QObject* parent)
: QObject(parent)
, m_compositor(compositor)
, m_pw(new PWState())
{
    m_captureTimer = new QTimer(this);
    connect(m_captureTimer, &QTimer::timeout,
            this, &ScreencastManager::onCaptureTick);

    #ifdef HAVE_PIPEWIRE
    m_pw->mgr = this;
    #endif
}

ScreencastManager::~ScreencastManager() {
    shutdown();
    delete m_pw;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inicjalizacja
// ─────────────────────────────────────────────────────────────────────────────

bool ScreencastManager::initialize() {
    #ifdef HAVE_PIPEWIRE
    pw_init(nullptr, nullptr);
    m_pwAvailable = initPipeWire();
    if (m_pwAvailable)
        qInfo() << "[Screencast] PipeWire zainicjalizowany, wersja:"
        << pw_get_library_version();
    else
        qWarning() << "[Screencast] PipeWire niedostępny — screen share wyłączony";
    #else
    m_pwAvailable = false;
    qInfo() << "[Screencast] skompilowany bez PipeWire — "
    "zrzuty ekranu i nagrywanie ffmpeg działają normalnie";
    #endif
    return true; // zawsze zwracamy true — screenshot działa bez PipeWire
}

void ScreencastManager::shutdown() {
    stopRecording();
    stopStream();
    cleanupPipeWire();
}

bool ScreencastManager::isAvailable() const {
    return m_pwAvailable;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zrzut ekranu (screenshot)
// ─────────────────────────────────────────────────────────────────────────────

QString ScreencastManager::takeScreenshot(const QString& path,
                                          const QRect&   region,
                                          const QString& format) {
    // Grabuj klatkę
    const QImage frame = captureFrame(region);
    if (frame.isNull()) {
        const QString err = "nie można pobrać klatki z kompozytora";
        qWarning() << "[Screenshot]" << err;
        emit screenshotFailed(err);
        return {};
    }

    // Wyznacz ścieżkę docelową
    QString outPath = path;
    if (outPath.isEmpty()) {
        const QString ext = format.isEmpty() ? "png" : format.toLower();
        outPath = defaultScreenshotDir() + "/" + timestampedFilename(ext);
    }

    // Utwórz katalog jeśli nie istnieje
    QDir().mkpath(QFileInfo(outPath).absolutePath());

    // Zapisz
    QImageWriter writer(outPath);
    writer.setFormat(format.isEmpty() ? "png" : format.toLower().toLocal8Bit());

    // Jakość dla JPG
    if (format.toLower() == "jpg" || format.toLower() == "jpeg")
        writer.setQuality(95);

    if (!writer.write(frame)) {
        const QString err = QString("zapis nieudany: %1").arg(writer.errorString());
        qWarning() << "[Screenshot]" << err;
        emit screenshotFailed(err);
        return {};
    }

    qInfo() << "[Screenshot] zapisano:" << outPath
    << frame.width() << "x" << frame.height();
    emit screenshotSaved(outPath);
    return outPath;
                                          }

                                          // ─────────────────────────────────────────────────────────────────────────────
                                          // Nagrywanie do pliku (przez ffmpeg)
                                          // ─────────────────────────────────────────────────────────────────────────────

                                          bool ScreencastManager::startRecording(const QString& outputPath,
                                                                                 int            fps,
                                                                                 const QRect&   region) {
                                              if (m_recording) {
                                                  qWarning() << "[Screencast] nagrywanie już trwa:" << m_recordingPath;
                                                  return false;
                                              }

                                              // Grabuj jedną klatkę żeby znać rozmiar
                                              const QImage probe = captureFrame(region);
                                              if (probe.isNull()) {
                                                  qWarning() << "[Screencast] nie można pobrać klatki testowej";
                                                  return false;
                                              }

                                              // Wyznacz ścieżkę
                                              QString outPath = outputPath;
                                              if (outPath.isEmpty())
                                                  outPath = defaultScreenshotDir() + "/" + timestampedFilename("mp4");

                                              QDir().mkpath(QFileInfo(outPath).absolutePath());

                                              // Sprawdź czy ffmpeg jest dostępny
                                              QProcess probe2;
                                              probe2.start("ffmpeg", {"-version"});
                                              probe2.waitForFinished(2000);
                                              if (probe2.exitCode() != 0) {
                                                  qWarning() << "[Screencast] ffmpeg nie znaleziony w PATH";
                                                  qWarning() << "  Zainstaluj: sudo apt install ffmpeg";
                                                  return false;
                                              }

                                              // Uruchom ffmpeg
                                              if (!startFFmpeg(outPath, probe.size(), fps)) return false;

                                              m_recordingPath  = outPath;
                                              m_captureFps     = fps;
                                              m_captureRegion  = region;
                                              m_recording      = true;
                                              m_frameCount     = 0;

                                              // Timer nagrywania
                                              m_captureTimer->setInterval(1000 / fps);
                                              m_captureTimer->start();

                                              qInfo() << "[Screencast] nagrywanie started:" << outPath
                                              << fps << "fps" << probe.size();
                                              emit recordingStarted(outPath);
                                              return true;
                                                                                 }

                                                                                 void ScreencastManager::stopRecording() {
                                                                                     if (!m_recording) return;

                                                                                     m_captureTimer->stop();
                                                                                     stopFFmpeg();

                                                                                     m_recording = false;
                                                                                     const QString path = m_recordingPath;
                                                                                     m_recordingPath.clear();
                                                                                     m_frameCount = 0;

                                                                                     qInfo() << "[Screencast] nagrywanie stopped:" << path;
                                                                                     emit recordingStopped(path);
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // PipeWire stream (screen share)
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 bool ScreencastManager::startStream(int fps) {
                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     if (m_streaming) return true;
                                                                                     if (!m_pwAvailable) {
                                                                                         qWarning() << "[Screencast] PipeWire niedostępny";
                                                                                         return false;
                                                                                     }

                                                                                     m_captureFps = fps;
                                                                                     m_captureTimer->setInterval(1000 / fps);
                                                                                     m_captureTimer->start();

                                                                                     m_streaming = true;
                                                                                     qInfo() << "[Screencast] PipeWire stream started, fps:" << fps;
                                                                                     return true;
                                                                                     #else
                                                                                     Q_UNUSED(fps);
                                                                                     qWarning() << "[Screencast] skompilowano bez PipeWire";
                                                                                     return false;
                                                                                     #endif
                                                                                 }

                                                                                 void ScreencastManager::stopStream() {
                                                                                     if (!m_streaming) return;

                                                                                     m_captureTimer->stop();
                                                                                     m_streaming = false;

                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     if (m_pw->stream) {
                                                                                         pw_stream_disconnect(m_pw->stream);
                                                                                     }
                                                                                     #endif

                                                                                     emit streamStopped();
                                                                                     qInfo() << "[Screencast] PipeWire stream stopped";
                                                                                 }

                                                                                 uint32_t ScreencastManager::pipeWireNodeId() const {
                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     return m_pw->nodeId;
                                                                                     #else
                                                                                     return 0;
                                                                                     #endif
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // Pętla przechwytywania
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 void ScreencastManager::onCaptureTick() {
                                                                                     const QImage frame = captureFrame(m_captureRegion);
                                                                                     if (frame.isNull()) return;

                                                                                     // Nagrywanie → ffmpeg
                                                                                     if (m_recording && m_ffmpegProc &&
                                                                                         m_ffmpegProc->state() == QProcess::Running) {
                                                                                         sendFrameToFFmpeg(frame);
                                                                                     ++m_frameCount;
                                                                                         }

                                                                                         // Screen share → PipeWire
                                                                                         if (m_streaming) {
                                                                                             pushFrameToPipeWire(frame);
                                                                                         }

                                                                                         // Emituj podgląd (np. do thumbnail w barze)
                                                                                         emit frameReady(frame);
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // captureFrame — grab z QOpenGLWidget
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 QImage ScreencastManager::captureFrame(const QRect& region) {
                                                                                     // Pobierz output (QOpenGLWidget) z kompozytora
                                                                                     WMOutput* output = m_compositor->primaryOutput();
                                                                                     if (!output) {
                                                                                         qWarning() << "[Screencast] brak primaryOutput";
                                                                                         return {};
                                                                                     }

                                                                                     // grabFramebuffer() — najszybszy sposób, działa na QOpenGLWidget
                                                                                     // Zwraca QImage w formacie ARGB32 z bieżącej klatki GL
                                                                                     QImage frame = output->grabFramebuffer();
                                                                                     if (frame.isNull()) {
                                                                                         // Fallback: render przez QScreen
                                                                                         if (QScreen* scr = output->screen()) {
                                                                                             frame = scr->grabWindow(0).toImage();
                                                                                         }
                                                                                     }
                                                                                     if (frame.isNull()) return {};

                                                                                     // Przytnij do regionu jeśli podany
                                                                                     if (!region.isEmpty() && region != QRect(0,0,frame.width(),frame.height())) {
                                                                                         const QRect clipped = region.intersected(frame.rect());
                                                                                         if (!clipped.isEmpty())
                                                                                             frame = frame.copy(clipped);
                                                                                     }

                                                                                     // Konwertuj do RGB32 (bez alpha) — ffmpeg i PipeWire preferują
                                                                                     return frame.convertToFormat(QImage::Format_RGB32);
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // PipeWire init / cleanup
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 bool ScreencastManager::initPipeWire() {
                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     // Utwórz thread loop
                                                                                     m_pw->loop = pw_thread_loop_new("hackerlandwm-screencast", nullptr);
                                                                                     if (!m_pw->loop) {
                                                                                         qWarning() << "[PipeWire] pw_thread_loop_new failed";
                                                                                         return false;
                                                                                     }

                                                                                     // Context
                                                                                     m_pw->context = pw_context_new(
                                                                                         pw_thread_loop_get_loop(m_pw->loop), nullptr, 0);
                                                                                     if (!m_pw->context) {
                                                                                         qWarning() << "[PipeWire] pw_context_new failed";
                                                                                         return false;
                                                                                     }

                                                                                     // Połącz z serwerem PipeWire
                                                                                     m_pw->core = pw_context_connect(m_pw->context, nullptr, 0);
                                                                                     if (!m_pw->core) {
                                                                                         qWarning() << "[PipeWire] cannot connect to PipeWire daemon";
                                                                                         qWarning() << "  Upewnij się że pipewire działa: systemctl --user status pipewire";
                                                                                         return false;
                                                                                     }

                                                                                     // Utwórz stream video/source
                                                                                     m_pw->stream = pw_stream_new(
                                                                                         m_pw->core,
                                                                                         "hackerlandwm-screen",
                                                                                         pw_properties_new(
                                                                                             PW_KEY_MEDIA_CLASS, "Video/Source",
                                                                                             PW_KEY_NODE_NAME,   "hackerlandwm",
                                                                                             nullptr));

                                                                                     if (!m_pw->stream) {
                                                                                         qWarning() << "[PipeWire] pw_stream_new failed";
                                                                                         return false;
                                                                                     }

                                                                                     // Zarejestruj callbacki
                                                                                     pw_stream_add_listener(m_pw->stream, new spa_hook{},
                                                                                                            &kStreamEvents, m_pw);

                                                                                     // Pobierz node ID (potrzebne do xdg-desktop-portal)
                                                                                     m_pw->nodeId = pw_stream_get_node_id(m_pw->stream);

                                                                                     // Uruchom loop
                                                                                     pw_thread_loop_start(m_pw->loop);

                                                                                     qInfo() << "[PipeWire] stream node ID:" << m_pw->nodeId;
                                                                                     return true;

                                                                                     #else
                                                                                     return false;
                                                                                     #endif
                                                                                 }

                                                                                 void ScreencastManager::cleanupPipeWire() {
                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     if (m_pw->loop)    pw_thread_loop_stop(m_pw->loop);
                                                                                     if (m_pw->stream)  { pw_stream_destroy(m_pw->stream);  m_pw->stream  = nullptr; }
                                                                                     if (m_pw->core)    { pw_core_disconnect(m_pw->core);   m_pw->core    = nullptr; }
                                                                                     if (m_pw->context) { pw_context_destroy(m_pw->context);m_pw->context = nullptr; }
                                                                                     if (m_pw->loop)    { pw_thread_loop_destroy(m_pw->loop);m_pw->loop   = nullptr; }
                                                                                     pw_deinit();
                                                                                     #endif
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // pushFrameToPipeWire — wysyła QImage jako pw_buffer
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 void ScreencastManager::pushFrameToPipeWire(const QImage& frame) {
                                                                                     #ifdef HAVE_PIPEWIRE
                                                                                     if (!m_pw->stream || !m_pw->connected) return;

                                                                                     pw_thread_loop_lock(m_pw->loop);

                                                                                     pw_buffer* buf = pw_stream_dequeue_buffer(m_pw->stream);
                                                                                     if (!buf) {
                                                                                         pw_thread_loop_unlock(m_pw->loop);
                                                                                         return;
                                                                                     }

                                                                                     spa_buffer* sbuf = buf->buffer;
                                                                                     if (!sbuf || !sbuf->datas[0].data) {
                                                                                         pw_stream_queue_buffer(m_pw->stream, buf);
                                                                                         pw_thread_loop_unlock(m_pw->loop);
                                                                                         return;
                                                                                     }

                                                                                     // Kopiuj dane RGBA do bufora PipeWire
                                                                                     const int stride    = frame.width() * 4;
                                                                                     const int totalSize = stride * frame.height();

                                                                                     if ((int)sbuf->datas[0].maxsize >= totalSize) {
                                                                                         // Konwertuj do RGBA8888 (PipeWire oczekuje tego formatu)
                                                                                         const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
                                                                                         memcpy(sbuf->datas[0].data, rgba.constBits(), totalSize);

                                                                                         sbuf->datas[0].chunk->offset = 0;
                                                                                         sbuf->datas[0].chunk->stride = stride;
                                                                                         sbuf->datas[0].chunk->size   = totalSize;
                                                                                     }

                                                                                     pw_stream_queue_buffer(m_pw->stream, buf);
                                                                                     pw_thread_loop_unlock(m_pw->loop);

                                                                                     #else
                                                                                     Q_UNUSED(frame);
                                                                                     #endif
                                                                                 }

                                                                                 // ─────────────────────────────────────────────────────────────────────────────
                                                                                 // FFmpeg helpers
                                                                                 // ─────────────────────────────────────────────────────────────────────────────

                                                                                 bool ScreencastManager::startFFmpeg(const QString& outPath,
                                                                                                                     const QSize&   size,
                                                                                                                     int            fps) {
                                                                                     // ffmpeg odczytuje surowe klatki RGBA z stdin i koduje do pliku
                                                                                     //
                                                                                     // Parametry:
                                                                                     //   -f rawvideo -pix_fmt rgb32     — surowe RGBA z pipe
                                                                                     //   -s WxH -r FPS                  — rozmiar i fps
                                                                                     //   -i pipe:0                       — stdin
                                                                                     //   -c:v libx264 -preset ultrafast — szybkie kodowanie H.264
                                                                                     //   -pix_fmt yuv420p               — kompatybilność z odtwarzaczami
                                                                                     //   -crf 23                        — jakość (niższy = lepszy, 18-28 OK)
                                                                                     //   -movflags +faststart           — streaming MP4
                                                                                     //   outPath                         — plik wyjściowy

                                                                                     const QString sizeStr = QString("%1x%2").arg(size.width()).arg(size.height());

                                                                                     const QStringList args = {
                                                                                         "-y",                      // nadpisz bez pytania
                                                                                         "-f",    "rawvideo",
                                                                                         "-pix_fmt", "rgb32",       // odpowiada QImage::Format_RGB32
                                                                                         "-s",    sizeStr,
                                                                                         "-r",    QString::number(fps),
                                                                                         "-i",    "pipe:0",         // stdin
                                                                                         "-c:v",  "libx264",
                                                                                         "-preset", "ultrafast",    // minimalny CPU overhead
                                                                                         "-crf",  "23",
                                                                                         "-pix_fmt", "yuv420p",
                                                                                         "-movflags", "+faststart",
                                                                                         outPath
                                                                                     };

                                                                                     m_ffmpegProc = new QProcess(this);
                                                                                     m_ffmpegProc->setProgram("ffmpeg");
                                                                                     m_ffmpegProc->setArguments(args);

                                                                                     // ffmpeg stderr → nasz debug log
                                                                                     m_ffmpegProc->setStandardErrorFile(
                                                                                         QDir::tempPath() + "/hackerlandwm-ffmpeg.log");

                                                                                     m_ffmpegProc->start();
                                                                                     if (!m_ffmpegProc->waitForStarted(3000)) {
                                                                                         qWarning() << "[Screencast] nie można uruchomić ffmpeg:"
                                                                                         << m_ffmpegProc->errorString();
                                                                                         delete m_ffmpegProc;
                                                                                         m_ffmpegProc = nullptr;
                                                                                         return false;
                                                                                     }

                                                                                     qInfo() << "[Screencast] ffmpeg uruchomiony, PID:"
                                                                                     << m_ffmpegProc->processId();
                                                                                     return true;
                                                                                                                     }

                                                                                                                     void ScreencastManager::stopFFmpeg() {
                                                                                                                         if (!m_ffmpegProc) return;

                                                                                                                         // Zamknij stdin — ffmpeg dokończy kodowanie i wyjdzie
                                                                                                                         m_ffmpegProc->closeWriteChannel();
                                                                                                                         if (!m_ffmpegProc->waitForFinished(10000)) {
                                                                                                                             qWarning() << "[Screencast] ffmpeg nie zakończył się — kill";
                                                                                                                             m_ffmpegProc->kill();
                                                                                                                             m_ffmpegProc->waitForFinished(2000);
                                                                                                                         }

                                                                                                                         qInfo() << "[Screencast] ffmpeg zakończył, exit code:"
                                                                                                                         << m_ffmpegProc->exitCode()
                                                                                                                         << "frames:" << m_frameCount;

                                                                                                                         delete m_ffmpegProc;
                                                                                                                         m_ffmpegProc = nullptr;
                                                                                                                     }

                                                                                                                     void ScreencastManager::sendFrameToFFmpeg(const QImage& frame) {
                                                                                                                         if (!m_ffmpegProc ||
                                                                                                                             m_ffmpegProc->state() != QProcess::Running) return;

                                                                                                                         // ffmpeg oczekuje surowych bajtów RGB32 (BGRA w little-endian)
                                                                                                                         // QImage::Format_RGB32 to właśnie BGRX — idealnie pasuje
                                                                                                                         const int bytes = frame.width() * frame.height() * 4;
                                                                                                                         const qint64 written = m_ffmpegProc->write(
                                                                                                                             reinterpret_cast<const char*>(frame.constBits()), bytes);

                                                                                                                         if (written != bytes) {
                                                                                                                             qWarning() << "[Screencast] ffmpeg write error, wrote"
                                                                                                                             << written << "of" << bytes << "bytes";
                                                                                                                         }
                                                                                                                     }

                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                     // Ścieżki plików
                                                                                                                     // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                     QString ScreencastManager::defaultScreenshotDir() {
                                                                                                                         // Szukaj po kolei: ~/Obrazy, ~/Pictures, ~/Desktop, ~
                                                                                                                         const QStringList candidates = {
                                                                                                                             QDir::homePath() + "/Obrazy",
                                                                                                                             QDir::homePath() + "/Pictures",
                                                                                                                             QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                                                                                             QDir::homePath() + "/Desktop",
                                                                                                                             QDir::homePath(),
                                                                                                                         };
                                                                                                                         for (const QString& c : candidates) {
                                                                                                                             if (QDir(c).exists()) return c;
                                                                                                                         }
                                                                                                                         // Utwórz ~/Obrazy jeśli nic nie istnieje
                                                                                                                         const QString dir = QDir::homePath() + "/Obrazy";
                                                                                                                         QDir().mkpath(dir);
                                                                                                                         return dir;
                                                                                                                     }

                                                                                                                     QString ScreencastManager::timestampedFilename(const QString& ext) {
                                                                                                                         const QString ts = QDateTime::currentDateTime()
                                                                                                                         .toString("yyyy-MM-dd_hh-mm-ss");
                                                                                                                         return QString("screenshot-%1.%2").arg(ts).arg(ext);
                                                                                                                     }
