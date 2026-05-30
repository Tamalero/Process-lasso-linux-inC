#include "gui/mainwindow.h"
#include "verbose.h"
#include <QApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <unistd.h>

bool gVerbose = false;

// Per-user socket name so multi-user machines don't collide.
static QString socketName()
{
    const QString user = qEnvironmentVariable("USER",
                            QString::number(getuid()));
    return QStringLiteral("process-lasso-qt-") + user;
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--verbose") { gVerbose = true; break; }

    if (gVerbose)
        fprintf(stderr, "[V] verbose mode enabled\n");

    // ── Single-instance guard ─────────────────────────────────────────────────
    // Try to reach an already-running instance before creating QApplication
    // (cheaper: no window system connection needed just to check).
    {
        QLocalSocket probe;
        probe.connectToServer(socketName());
        if (probe.waitForConnected(300)) {
            // First instance is alive — ask it to raise itself and exit.
            probe.write("raise\n");
            probe.flush();
            probe.waitForBytesWritten(300);
            return 0;
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("process-lasso-qt"));
    app.setApplicationDisplayName(QStringLiteral("Process Lasso Qt"));
    app.setOrganizationName(QStringLiteral("AcornInteractive"));
    app.setOrganizationDomain(QStringLiteral("acorninteractive.ca"));

    // Use the installed themed icon when available
    QIcon appIcon = QIcon::fromTheme(QStringLiteral("process-lasso"),
                        QIcon(QStringLiteral(":/icons/process-lasso.png")));
    app.setWindowIcon(appIcon);

    // ── Local server (first instance only) ───────────────────────────────────
    QLocalServer server;
    // Remove a stale socket left by a previous crash.
    QLocalServer::removeServer(socketName());
    server.listen(socketName());

    MainWindow win(&app);
    win.show();

    // When a second launch connects, show and raise the window.
    QObject::connect(&server, &QLocalServer::newConnection, [&]{
        QLocalSocket *sock = server.nextPendingConnection();
        QObject::connect(sock, &QLocalSocket::readyRead, [sock, &win]{
            sock->readAll(); // consume the "raise\n" message
            win.show();
            win.raise();
            win.activateWindow();
            sock->deleteLater();
        });
    });

    return app.exec();
}
