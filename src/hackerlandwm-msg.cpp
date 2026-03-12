// ─────────────────────────────────────────────────────────────────────────────
// hackerlandwm-msg — IPC klient dla HackerLand WM
//
// Kompilacja (osobny target w CMakeLists):
//   add_executable(hackerlandwm-msg src/hackerlandwm-msg.cpp)
//   target_link_libraries(hackerlandwm-msg PRIVATE Qt6::Core Qt6::Network)
//
// Użycie:
//   hackerlandwm-msg workspace 3
//   hackerlandwm-msg exec alacritty
//   hackerlandwm-msg exec "firefox --new-window"
//   hackerlandwm-msg layout spiral
//   hackerlandwm-msg layout bsp
//   hackerlandwm-msg close
//   hackerlandwm-msg fullscreen
//   hackerlandwm-msg float
//   hackerlandwm-msg maximize
//   hackerlandwm-msg focus left
//   hackerlandwm-msg focus right
//   hackerlandwm-msg reload
//   hackerlandwm-msg lock
//   hackerlandwm-msg status
//   hackerlandwm-msg quit
//
// Protokół: linia tekstu → JSON response {"success":true} lub {"success":false,"error":"..."}
// Socket:   $XDG_RUNTIME_DIR/hackerlandwm.sock
// ─────────────────────────────────────────────────────────────────────────────

#include <QCoreApplication>
#include <QLocalSocket>
#include <QTextStream>
#include <QStringList>
#include <QTimer>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
// Socket path — musi zgadzać się z IPCServer::socketPath()
// ─────────────────────────────────────────────────────────────────────────────
static QString socketPath() {
    QString rdir = qgetenv("XDG_RUNTIME_DIR");
    if (rdir.isEmpty())
        rdir = QString("/run/user/%1").arg((int)getuid());
    return rdir + "/hackerlandwm.sock";
}

// ─────────────────────────────────────────────────────────────────────────────
// Pomoc
// ─────────────────────────────────────────────────────────────────────────────
static void printHelp() {
    fprintf(stdout,
            "hackerlandwm-msg — sterowanie HackerLand WM przez IPC\n"
            "\n"
            "Użycie:\n"
            "  hackerlandwm-msg <komenda> [argumenty]\n"
            "\n"
            "Komendy:\n"
            "  workspace <N>          przełącz na workspace N (1-9)\n"
            "  exec <komenda>         uruchom program\n"
            "  layout <nazwa>         zmień układ: spiral|bsp|tall|wide|grid|dwindle|monocle\n"
            "  focus <kierunek>       przenieś focus: left|right|up|down\n"
            "  move <kierunek>        przesuń okno: left|right|up|down\n"
            "  close                  zamknij aktywne okno\n"
            "  fullscreen             przełącz fullscreen\n"
            "  float                  przełącz floating\n"
            "  maximize               przełącz maximize\n"
            "  reload                 przeładuj config bez restartu WM\n"
            "  lock                   zablokuj ekran\n"
            "  status                 pokaż stan WM (JSON)\n"
            "  quit                   zamknij WM\n"
            "\n"
            "Przykłady:\n"
            "  hackerlandwm-msg workspace 3\n"
            "  hackerlandwm-msg exec alacritty\n"
            "  hackerlandwm-msg exec \"firefox --new-window\"\n"
            "  hackerlandwm-msg layout spiral\n"
            "  hackerlandwm-msg reload\n"
            "  hackerlandwm-msg lock\n"
            "  hackerlandwm-msg status\n"
            "\n"
            "Socket: %s\n",
            socketPath().toLocal8Bit().constData()
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// Walidacja komendy przed wysłaniem
// ─────────────────────────────────────────────────────────────────────────────
static bool validateCommand(const QStringList& parts, QString& error) {
    if (parts.isEmpty()) { error = "pusta komenda"; return false; }

    const QString verb = parts[0].toLower();

    if (verb == "workspace") {
        if (parts.size() < 2) { error = "workspace wymaga numeru (1-9)"; return false; }
        bool ok; int n = parts[1].toInt(&ok);
        if (!ok || n < 1 || n > 99) { error = "nieprawidłowy numer workspace"; return false; }
    } else if (verb == "exec") {
        if (parts.size() < 2) { error = "exec wymaga komendy"; return false; }
    } else if (verb == "layout") {
        if (parts.size() < 2) { error = "layout wymaga nazwy"; return false; }
        const QStringList valid = {"spiral","bsp","tall","wide","grid","dwindle","monocle","centered","threecolumn"};
        if (!valid.contains(parts[1].toLower())) {
            error = QString("nieznany layout '%1'. Dostępne: %2")
            .arg(parts[1]).arg(valid.join(", "));
            return false;
        }
    } else if (verb == "focus" || verb == "move") {
        if (parts.size() < 2) { error = verb + " wymaga kierunku: left|right|up|down"; return false; }
        const QStringList dirs = {"left","right","up","down","prev","next"};
        if (!dirs.contains(parts[1].toLower())) {
            error = "nieznany kierunek: " + parts[1];
            return false;
        }
    } else if (!QStringList{"close","fullscreen","float","maximize",
        "reload","lock","status","quit"}.contains(verb)) {
        error = "nieznana komenda: " + verb;
        return false;
        }

        return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Drukowanie odpowiedzi JSON
// ─────────────────────────────────────────────────────────────────────────────
static int printResponse(const QByteArray& raw) {
    if (raw.trimmed().isEmpty()) {
        fprintf(stderr, "Brak odpowiedzi od hackerlandwm\n");
        return 1;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.trimmed(), &err);

    if (err.error != QJsonParseError::NoError) {
        // Odpowiedź nie jest JSON — wydrukuj bezpośrednio
        fprintf(stdout, "%s\n", raw.trimmed().constData());
        return 0;
    }

    const QJsonObject obj = doc.object();
    const bool success    = obj["success"].toBool();

    if (success) {
        if (obj.contains("result")) {
            // Sformatuj wynik (może być zagnieżdżony JSON)
            const QJsonValue result = obj["result"];
            if (result.isObject() || result.isArray()) {
                fprintf(stdout, "%s\n",
                        QJsonDocument(result.isObject()
                        ? QJsonDocument(result.toObject())
                        : QJsonDocument(result.toArray()))
                        .toJson(QJsonDocument::Indented).constData());
            } else {
                fprintf(stdout, "%s\n", result.toString().toLocal8Bit().constData());
            }
        } else {
            fprintf(stdout, "OK\n");
        }
        return 0;
    } else {
        fprintf(stderr, "Błąd: %s\n",
                obj["error"].toString("nieznany błąd").toLocal8Bit().constData());
        return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("hackerlandwm-msg");
    app.setApplicationVersion("1.0.0");

    const QStringList allArgs = app.arguments().mid(1);

    // ── Pomoc ─────────────────────────────────────────────────────────────
    if (allArgs.isEmpty() ||
        allArgs.contains("-h") ||
        allArgs.contains("--help")) {
        printHelp();
    return allArgs.isEmpty() ? 1 : 0;
        }

        if (allArgs.contains("--version") || allArgs.contains("-v")) {
            fprintf(stdout, "hackerlandwm-msg 1.0.0\n");
            return 0;
        }

        // ── Zbuduj komendę ────────────────────────────────────────────────────
        const QString cmd = allArgs.join(' ');

        // ── Walidacja ─────────────────────────────────────────────────────────
        QString validErr;
        if (!validateCommand(allArgs, validErr)) {
            fprintf(stderr, "hackerlandwm-msg: %s\n", validErr.toLocal8Bit().constData());
            fprintf(stderr, "Użyj --help aby zobaczyć dostępne komendy.\n");
            return 1;
        }

        // ── Połącz z WM ───────────────────────────────────────────────────────
        const QString path = socketPath();
        QLocalSocket sock;
        sock.connectToServer(path);

        if (!sock.waitForConnected(2000)) {
            fprintf(stderr,
                    "hackerlandwm-msg: nie można połączyć z %s\n"
                    "  Czy hackerlandwm działa?\n"
                    "  Sprawdź: ls -la %s\n",
                    path.toLocal8Bit().constData(),
                    path.toLocal8Bit().constData());
            return 1;
        }

        // ── Wyślij komendę ────────────────────────────────────────────────────
        sock.write((cmd + "\n").toUtf8());
        if (!sock.flush()) {
            fprintf(stderr, "hackerlandwm-msg: błąd wysyłania\n");
            return 1;
        }

        // ── Czekaj na odpowiedź ───────────────────────────────────────────────
        // Komenda "quit" kończy WM — nie czekaj na odpowiedź
        if (allArgs[0].toLower() == "quit") {
            fprintf(stdout, "OK\n");
            return 0;
        }

        if (!sock.waitForReadyRead(3000)) {
            fprintf(stderr, "hackerlandwm-msg: timeout — brak odpowiedzi\n");
            return 1;
        }

        const QByteArray response = sock.readAll();
        sock.disconnectFromServer();

        return printResponse(response);
}
