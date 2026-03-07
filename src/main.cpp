#include <QApplication>
#include <QLoggingCategory>
#include <QCommandLineParser>
#include <QDir>
#include <csignal>

#include "compositor/WMCompositor.h"
#include "core/Config.h"

Q_LOGGING_CATEGORY(lcWM, "hackerlandwm")

static WMCompositor* g_compositor = nullptr;

static void signalHandler(int sig) {
    qCInfo(lcWM) << "Received signal" << sig << "- shutting down gracefully";
    if (g_compositor)
        QMetaObject::invokeMethod(g_compositor, "shutdown", Qt::QueuedConnection);
    QCoreApplication::quit();
}

int main(int argc, char* argv[]) {
    // ── Environment — must be set before QApplication ─────────────────────
    qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");

    // We ARE the Wayland compositor running on TTY (bare metal, no X11,
    // no parent Wayland session).
    //
    // QT_QPA_PLATFORM=eglfs   — renders directly to GPU/KMS/DRM (best)
    // QT_QPA_PLATFORM=linuxfb — renders to /dev/fb0 (fallback, no GPU accel)
    //
    // Do NOT use "xcb"     — needs X11 display server
    // Do NOT use "wayland" — tries to connect to a parent compositor
    //
    // Only override if the user hasn't forced something via environment.
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        // Try eglfs first (GPU direct rendering on TTY).
        // User can override: QT_QPA_PLATFORM=linuxfb ./hackerlandwm
        qputenv("QT_QPA_PLATFORM", "eglfs");
    }

    // Our compositor will listen on this Wayland socket.
    // Clients set WAYLAND_DISPLAY=wayland-0 to connect to us.
    if (qgetenv("WAYLAND_DISPLAY").isEmpty()) {
        qputenv("WAYLAND_DISPLAY", "wayland-0");
    }

    // eglfs on KMS needs this to pick the right GPU
    if (qgetenv("QT_QPA_EGLFS_KMS_ATOMIC").isEmpty()) {
        qputenv("QT_QPA_EGLFS_KMS_ATOMIC", "1");
    }

    QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName("HackerLand WM");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("HackerOS");
    app.setOrganizationDomain("hackeros.io");

    // ── Command-line ──────────────────────────────────────────────────────
    QCommandLineParser parser;
    parser.setApplicationDescription("HackerLand Tiling Wayland Compositor");
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption configOption({"c", "config"},
                                          "Path to config file", "file");
    const QCommandLineOption debugOption({"d", "debug"},
                                         "Enable verbose debug logging");
    const QCommandLineOption noAnimOption("no-animations",
                                          "Disable all animations");
    const QCommandLineOption fbOption("linuxfb",
                                      "Force linuxfb backend (software rendering, no GPU required)");

    parser.addOption(configOption);
    parser.addOption(debugOption);
    parser.addOption(noAnimOption);
    parser.addOption(fbOption);
    parser.process(app);

    // --linuxfb flag: switch to software framebuffer renderer
    if (parser.isSet(fbOption)) {
        qputenv("QT_QPA_PLATFORM", "linuxfb");
    }

    if (parser.isSet(debugOption)) {
        QLoggingCategory::setFilterRules(
            "hackerlandwm.debug=true\n"
            "hackerlandwm.warning=true\n"
            "qt.qpa.*=true");
    }

    // ── Config ────────────────────────────────────────────────────────────
    QString configPath = parser.value(configOption);
    if (configPath.isEmpty()) {
        configPath = QDir::homePath() + "/.config/hackerlandwm/config.toml";
    }

    Config& config = Config::instance();
    config.load(configPath);

    if (parser.isSet(noAnimOption)) {
        config.setAnimationsEnabled(false);
    }

    // ── Signal handlers ───────────────────────────────────────────────────
    signal(SIGTERM, signalHandler);
    signal(SIGINT,  signalHandler);
    signal(SIGHUP,  signalHandler);

    // ── Compositor ────────────────────────────────────────────────────────
    qCInfo(lcWM) << "Starting HackerLand WM";
    qCInfo(lcWM) << "QT_QPA_PLATFORM  =" << qgetenv("QT_QPA_PLATFORM");
    qCInfo(lcWM) << "WAYLAND_DISPLAY  =" << qgetenv("WAYLAND_DISPLAY");
    qCInfo(lcWM) << "XDG_RUNTIME_DIR  =" << qgetenv("XDG_RUNTIME_DIR");

    WMCompositor compositor;
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        qCritical(lcWM) << "Failed to initialize compositor — aborting";
        return 1;
    }

    compositor.show();

    qCInfo(lcWM) << "HackerLand WM running — Wayland socket:"
    << qgetenv("WAYLAND_DISPLAY");

    return app.exec();
}
