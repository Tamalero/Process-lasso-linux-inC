#include "cpupark.h"
#include "cputopology.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

static QString helperPath() { return QStringLiteral("/usr/local/bin/process-lasso-helper"); }

namespace CpuPark {

bool isHelperInstalled()
{
    return QFile::exists(helperPath()) &&
           QFileInfo(helperPath()).isExecutable();
}

bool isSudoersInstalled()
{
    if (!isHelperInstalled()) return false;
    QProcess p;
    p.start(QStringLiteral("sudo"),
            QStringList() << QStringLiteral("-n") << helperPath()
                          << QStringLiteral("--check-only"));
    p.waitForFinished(3000);
    const int rc = p.exitCode();
    return rc == 0 || rc == 1;
}

bool isHelperCurrent()
{
    if (!isHelperInstalled()) return false;
    QFile f(helperPath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.readAll().contains("renice-pid");
}

std::pair<bool, QString> installHelper(const QString &username)
{
    Q_UNUSED(username)

    // Search standard data locations first (system/user install).
    QString script = QStandardPaths::locate(
        QStandardPaths::AppDataLocation,
        QStringLiteral("install-helper.sh"));

    // Fallback: look relative to the application binary.
    // Works for AppImage (binary lives at $APPDIR/usr/bin/) and portable builds.
    if (script.isEmpty()) {
        const QString candidate =
            QFileInfo(QCoreApplication::applicationDirPath() +
                      QStringLiteral("/../share/process-lasso-qt/install-helper.sh"))
            .absoluteFilePath();
        if (QFile::exists(candidate))
            script = candidate;
    }

    if (script.isEmpty())
        return {false, QStringLiteral("install-helper.sh not found in data directory.")};

    QProcess p;
    p.start(QStringLiteral("pkexec"),
            QStringList{QStringLiteral("bash"), script});
    if (!p.waitForFinished(30000))
        return {false, QStringLiteral("pkexec timed out.")};
    if (p.exitCode() == 0)
        return {true, QStringLiteral("Helper and sudoers rule installed.")};
    return {false, QStringLiteral("Install failed:\n") +
                   QString::fromUtf8(p.readAllStandardError())};
}

static std::pair<bool, QString> runHelper(const QStringList &args)
{
    if (!isHelperInstalled())
        return {false, QStringLiteral("Helper not installed.")};
    QProcess p;
    p.start(QStringLiteral("sudo"),
            QStringList(QStringList() << helperPath()) + args);
    if (!p.waitForFinished(10000))
        return {false, QStringLiteral("Helper timed out.")};
    if (p.exitCode() == 0) return {true, {}};
    return {false, QString::fromUtf8(p.readAllStandardError() + p.readAllStandardOutput()).trimmed()};
}

bool parkCpus(const QSet<int> &cpus,
              std::function<void(const QString &)> logCb)
{
    if (cpus.isEmpty()) return true;
    bool ok = true;
    auto sorted = cpus.values();
    std::sort(sorted.begin(), sorted.end());
    for (int cpu : sorted) {
        if (cpu == 0) {
            if (logCb) logCb(QStringLiteral("[Park] Skipping CPU 0 (bootstrap processor)"));
            continue;
        }
        auto [success, msg] = runHelper({QStringLiteral("cpu-online"),
                                          QString::number(cpu),
                                          QStringLiteral("0")});
        if (success) {
            if (logCb) logCb(QStringLiteral("[Park] CPU %1 → offline").arg(cpu));
        } else {
            if (logCb) logCb(QStringLiteral("[Park] CPU %1 FAILED: %2").arg(cpu).arg(msg));
            ok = false;
        }
    }
    return ok;
}

bool unParkAll(std::function<void(const QString &)> logCb)
{
    const auto offline = getOfflineCpuSet();
    if (offline.isEmpty()) {
        if (logCb) logCb(QStringLiteral("[Park] No offline CPUs to restore."));
        return true;
    }
    auto [ok, msg] = runHelper({QStringLiteral("cpu-unpark-all")});
    if (ok) {
        if (logCb) {
            auto sorted = offline.values();
            std::sort(sorted.begin(), sorted.end());
            QStringList nums;
            for (int c : sorted) nums << QString::number(c);
            logCb(QStringLiteral("[Park] CPUs %1 restored online.").arg(nums.join(',')));
        }
    } else {
        if (logCb) logCb(QStringLiteral("[Park] Unpark all FAILED: %1").arg(msg));
    }
    return ok;
}

bool setProcessNiceViaHelper(int pid, int niceVal)
{
    auto [ok, msg] = runHelper({QStringLiteral("renice-pid"),
                                 QString::number(niceVal),
                                 QString::number(pid)});
    return ok;
}

} // namespace CpuPark
