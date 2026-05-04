#include "gui/mainwindow.h"
#include "verbose.h"
#include <QApplication>
#include <QIcon>

bool gVerbose = false;

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--verbose") { gVerbose = true; break; }

    if (gVerbose)
        fprintf(stderr, "[V] verbose mode enabled\n");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("process-lasso-qt"));
    app.setApplicationDisplayName(QStringLiteral("Process Lasso Qt"));
    app.setOrganizationName(QStringLiteral("AcornInteractive"));
    app.setOrganizationDomain(QStringLiteral("acorninteractive.ca"));

    // Use the installed themed icon when available
    QIcon appIcon = QIcon::fromTheme(QStringLiteral("process-lasso"),
                        QIcon(QStringLiteral(":/icons/process-lasso.png")));
    app.setWindowIcon(appIcon);

    MainWindow win(&app);
    win.show();

    return app.exec();
}
