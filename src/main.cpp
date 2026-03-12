// ─────────────────────────────────────────────────────────────────────────────
// HackerLand WM — main.cpp
// Supports: x86 / i686 / x86_64
// ─────────────────────────────────────────────────────────────────────────────
#include <QApplication>
#include <QLoggingCategory>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QDateTime>
#include <QTimer>
#include <csignal>
#include <unistd.h>
#include <sys/utsname.h>

#include "compositor/WMCompositor.h"
#include "compositor/IPCServer.h"
#include "core/Config.h"
#include "core/GamepadHandler.h"

Q_LOGGING_CATEGORY(lcWM, "hackerlandwm")

// ── Global state ──────────────────────────────────────────────────────────────
static WMCompositor* g_compositor = nullptr;
static QFile*        g_logFile    = nullptr;
static QTextStream*  g_logStream  = nullptr;

// ── Log handler (--dev mode) ──────────────────────────────────────────────────
static void logHandler(QtMsgType type,
                       const QMessageLogContext&,
                       const QString& msg)
{
    const char* tag = "[ info]";
    switch (type) {
        case QtDebugMsg:    tag = "[debug]"; break;
        case QtWarningMsg:  tag = "[ warn]"; break;
        case QtCriticalMsg: tag = "[ CRIT]"; break;
        case QtFatalMsg:    tag = "[FATAL]"; break;
        default:            break;
    }

    const QByteArray line = QString("%1 %2 %3\n")
    .arg(QDateTime::currentDateTime().toString("hh:mm:ss.zzz"))
    .arg(tag)
    .arg(msg)
    .toLocal8Bit();

    fwrite(line.constData(), 1, line.size(), stderr);

    if (g_logStream) {
        *g_logStream << line;
        g_logStream->flush();
    }

    if (type == QtFatalMsg) {
        abort();
    }
}

// ── Signal handler ────────────────────────────────────────────────────────────
static void sigHandler(int sig)
{
    fprintf(stderr, "[hackerlandwm] signal %d\n", sig);

    if (g_compositor) {
        QMetaObject::invokeMethod(g_compositor,
                                  "shutdown",
                                  Qt::QueuedConnection);
    }

    QCoreApplication::quit();
}

// ── CPU / arch info ───────────────────────────────────────────────────────────
static void logCPUInfo()
{
    struct utsname u;
    if (uname(&u) == 0) {
        qCInfo(lcWM) << "arch:" << u.machine << "kernel:" << u.release;
    }
}

// ── TTY environment ───────────────────────────────────────────────────────────
// KEY FIX: QT_QPA_EGLFS_KMS_ATOMIC=1 enables atomic modesetting so the kernel
// handles VT switching even while we own the display.
// Without it: Ctrl+Alt+Fx freezes the system.
static void setupTTYEnv(bool forceLinuxfb)
{
    if (forceLinuxfb) {
        qputenv("QT_QPA_PLATFORM", "linuxfb:fb=/dev/fb0");
    } else {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "eglfs");
        }
        qputenv("QT_QPA_EGLFS_KMS_ATOMIC",      "1");
        qputenv("QT_QPA_EGLFS_ALWAYS_SET_MODE",  "1");
        qputenv("QT_QPA_EGLFS_HIDECURSOR",       "0");
    }

    if (qgetenv("WAYLAND_DISPLAY").isEmpty()) {
        qputenv("WAYLAND_DISPLAY", "wayland-0");
    }

    if (qgetenv("XDG_RUNTIME_DIR").isEmpty()) {
        qputenv("XDG_RUNTIME_DIR",
                QString("/run/user/%1").arg(static_cast<int>(getuid()))
                                      .toLocal8Bit());
    }

    // Environment hints for child Wayland clients
    qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    qputenv("MOZ_ENABLE_WAYLAND",          "1");
    qputenv("SDL_VIDEODRIVER",             "wayland");
    qputenv("GDK_BACKEND",                 "wayland");
    qputenv("CLUTTER_BACKEND",             "wayland");
    qputenv("_JAVA_AWT_WM_NONREPARENTING", "1");
}

// ── Cage mode ─────────────────────────────────────────────────────────────────
static int runCage(const CageConfig& cage, QApplication& app)
{
    if (cage.exec.isEmpty()) {
        qCritical(lcWM) << "[cage] exec not set — use --exec or set cage.exec in config";
        return 1;
    }

    qCInfo(lcWM) << "[cage] exec:" << cage.exec;

    WMCompositor compositor;
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        return 1;
    }

    compositor.show();

    QProcess proc;
    proc.setProcessChannelMode(QProcess::ForwardedChannels);

    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_DISPLAY", qgetenv("WAYLAND_DISPLAY"));
    env.insert("QT_QPA_PLATFORM", "wayland");
    proc.setProcessEnvironment(env);

    QObject::connect(&proc,
                     QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [&cage](int code, QProcess::ExitStatus) {
                         qCInfo(lcWM) << "[cage] process exited, code" << code;
                         if (cage.exitOnClose) {
                             QCoreApplication::quit();
                         }
                     });

    proc.start("bash", {"-c", cage.exec});
    return app.exec();
}

// ── Gamescope mode ────────────────────────────────────────────────────────────
static int runGamescope(const GamescopeConfig& gs, QApplication& app)
{
    if (gs.exec.isEmpty()) {
        qCritical(lcWM) << "[gamescope] exec not set";
        return 1;
    }

    qCInfo(lcWM) << "[gamescope] exec:" << gs.exec;

    WMCompositor compositor;
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        return 1;
    }

    compositor.show();

    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_DISPLAY",  qgetenv("WAYLAND_DISPLAY"));
    env.insert("QT_QPA_PLATFORM",  "wayland");

    if (gs.renderW  > 0) env.insert("HACKERLAND_RENDER_W",  QString::number(gs.renderW));
    if (gs.renderH  > 0) env.insert("HACKERLAND_RENDER_H",  QString::number(gs.renderH));
    if (gs.outputW  > 0) env.insert("HACKERLAND_OUTPUT_W",  QString::number(gs.outputW));
    if (gs.outputH  > 0) env.insert("HACKERLAND_OUTPUT_H",  QString::number(gs.outputH));
    if (gs.fpsLimit > 0) env.insert("HACKERLAND_FPS_LIMIT", QString::number(gs.fpsLimit));

    env.insert("HACKERLAND_FILTER",        gs.filter);
    env.insert("HACKERLAND_INTEGER_SCALE", gs.integerScale ? "1" : "0");

    QProcess proc;
    proc.setProcessChannelMode(QProcess::ForwardedChannels);
    proc.setProcessEnvironment(env);

    QObject::connect(&proc,
                     QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [&gs](int code, QProcess::ExitStatus) {
                         qCInfo(lcWM) << "[gamescope] process exited, code" << code;
                         if (gs.exitOnClose) {
                             QCoreApplication::quit();
                         }
                     });

    proc.start("bash", {"-c", gs.exec});
    return app.exec();
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // Scan for --linuxfb before QApplication initialises QPA
    bool earlyLinuxfb = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--linuxfb") == 0) {
            earlyLinuxfb = true;
            break;
        }
    }

    setupTTYEnv(earlyLinuxfb);
    QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName("HackerLand WM");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("HackerOS");
    app.setOrganizationDomain("hackeros.io");

    // ── CLI parser ────────────────────────────────────────────────────────
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "HackerLand WM — Wayland Tiling Compositor\n"
        "Config: ~/.config/hackeros/hackerland/config.toml");
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption cfgOpt    ({"c","config"},  "Path to config.toml",       "file");
    const QCommandLineOption devOpt    ("dev",           "Dev mode: write log to ~/hackerlandwm-DATE.log");
    const QCommandLineOption dbgOpt    ({"d","debug"},   "Verbose Qt debug output");
    const QCommandLineOption noAnimOpt ("no-animations", "Disable all animations");
    const QCommandLineOption fbOpt     ("linuxfb",       "Use software framebuffer (no GPU required)");
    const QCommandLineOption modeOpt   ("mode",          "Compositor mode: tiling|cage|gamescope", "mode");
    const QCommandLineOption execOpt   ("exec",          "Command to run in cage/gamescope mode",  "cmd");
    const QCommandLineOption gamepadOpt("gamepad",       "Enable gamepad / joystick input");
    const QCommandLineOption noIpcOpt  ("no-ipc",        "Disable IPC socket");

    parser.addOption(cfgOpt);
    parser.addOption(devOpt);
    parser.addOption(dbgOpt);
    parser.addOption(noAnimOpt);
    parser.addOption(fbOpt);
    parser.addOption(modeOpt);
    parser.addOption(execOpt);
    parser.addOption(gamepadOpt);
    parser.addOption(noIpcOpt);
    parser.process(app);

    // ── Dev logging ───────────────────────────────────────────────────────
    if (parser.isSet(devOpt)) {
        const QString ts   = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        const QString path = QDir::homePath() + "/hackerlandwm-" + ts + ".log";

        g_logFile = new QFile(path);
        if (g_logFile->open(QIODevice::WriteOnly | QIODevice::Text)) {
            g_logStream = new QTextStream(g_logFile);
            qInstallMessageHandler(logHandler);
            fprintf(stderr, "[dev] logging to: %s\n",
                    path.toLocal8Bit().constData());
        } else {
            fprintf(stderr, "[dev] cannot open log file: %s\n",
                    path.toLocal8Bit().constData());
            delete g_logFile;
            g_logFile = nullptr;
        }

        QLoggingCategory::setFilterRules(
            "hackerlandwm.debug=true\n"
            "hackerlandwm.warning=true\n"
            "qt.qpa.*=true");
    }

    if (parser.isSet(dbgOpt)) {
        QLoggingCategory::setFilterRules(
            "hackerlandwm.debug=true\n"
            "hackerlandwm.warning=true");
    }

    // ── Load config ───────────────────────────────────────────────────────
    const QString cfgPath = parser.isSet(cfgOpt)
    ? parser.value(cfgOpt)
    : Config::defaultConfigPath();

    Config& cfg = Config::instance();
    cfg.load(cfgPath);

    if (parser.isSet(noAnimOpt)) {
        cfg.setAnimationsEnabled(false);
    }

    if (parser.isSet(modeOpt)) {
        const QString m = parser.value(modeOpt).toLower();
        if      (m == "cage")      cfg.mode = CompositorMode::Cage;
        else if (m == "gamescope") cfg.mode = CompositorMode::Gamescope;
        else                       cfg.mode = CompositorMode::Tiling;
    }

    if (parser.isSet(execOpt)) {
        cfg.cage.exec      = parser.value(execOpt);
        cfg.gamescope.exec = parser.value(execOpt);
    }

    // ── Signals ───────────────────────────────────────────────────────────
    signal(SIGTERM, sigHandler);
    signal(SIGINT,  sigHandler);
    signal(SIGHUP,  sigHandler);

    // ── Startup log ───────────────────────────────────────────────────────
    logCPUInfo();
    qCInfo(lcWM) << "=== HackerLand WM" << app.applicationVersion() << "===";
    qCInfo(lcWM) << "mode            :"
    << (cfg.mode == CompositorMode::Cage      ? "cage"
    : cfg.mode == CompositorMode::Gamescope  ? "gamescope"
    :                                          "tiling");
    qCInfo(lcWM) << "config          :" << cfgPath;
    qCInfo(lcWM) << "QT_QPA_PLATFORM :" << qgetenv("QT_QPA_PLATFORM");
    qCInfo(lcWM) << "WAYLAND_DISPLAY :" << qgetenv("WAYLAND_DISPLAY");
    qCInfo(lcWM) << "XDG_RUNTIME_DIR :" << qgetenv("XDG_RUNTIME_DIR");

    // ── Mode dispatch ─────────────────────────────────────────────────────
    if (cfg.mode == CompositorMode::Cage) {
        return runCage(cfg.cage, app);
    }
    if (cfg.mode == CompositorMode::Gamescope) {
        return runGamescope(cfg.gamescope, app);
    }

    // ── Tiling WM ─────────────────────────────────────────────────────────
    WMCompositor compositor;
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        qCritical(lcWM) << "Compositor failed to initialise.";
        qCritical(lcWM) << "  Fallback: ./hackerlandwm --linuxfb";
        qCritical(lcWM) << "  KMS cfg:  QT_QPA_EGLFS_KMS_CONFIG=/etc/kms.json ./hackerlandwm";
        return 1;
    }

    // ── IPC server ────────────────────────────────────────────────────────
    IPCServer* ipc = nullptr;
    if (!parser.isSet(noIpcOpt)) {
        ipc = new IPCServer(&compositor, &compositor);
        if (!ipc->start()) {
            qCWarning(lcWM) << "[IPC] socket failed — hackerlandwm-msg will not work";
        } else {
            qCInfo(lcWM) << "IPC socket:" << IPCServer::socketPath();
        }
    }

    // ── Gamepad ───────────────────────────────────────────────────────────
    GamepadHandler* gamepad = nullptr;
    if (parser.isSet(gamepadOpt)) {
        gamepad = new GamepadHandler(&compositor);
        if (gamepad->start()) {
            // dispatchAction is the public slot — handleKeybind is private
            QObject::connect(gamepad, &GamepadHandler::actionTriggered,
                             &compositor, &WMCompositor::dispatchAction);
        } else {
            qCWarning(lcWM) << "[Gamepad] no joystick device found (/dev/input/js0-js3)";
        }
    }

    compositor.show();
    qCInfo(lcWM) << "Running. WAYLAND_DISPLAY=" << qgetenv("WAYLAND_DISPLAY");

    const int ret = app.exec();

    // ── Cleanup ───────────────────────────────────────────────────────────
    delete gamepad;
    delete ipc;

    if (g_logStream) {
        delete g_logStream;
        g_logStream = nullptr;
    }
    if (g_logFile) {
        g_logFile->close();
        delete g_logFile;
        g_logFile = nullptr;
    }

    return ret;
}
