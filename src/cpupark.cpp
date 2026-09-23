#include "cpupark.h"
#include "cputopology.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
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

    // Finding this script has been wrong twice. Both causes, written down so
    // they are not reintroduced:
    //
    // 1. AppDataLocation appends <organizationName>/<applicationName>, i.e.
    //    share/AcornInteractive/process-lasso-qt -- but CMake installs the
    //    script to share/process-lasso-qt, with no organisation directory. That
    //    lookup could never match, in ANY layout. GenericDataLocation with an
    //    explicit "process-lasso-qt/" prefix is the one that works, and it
    //    covers the AppImage too, because AppRun prepends $APPDIR/usr/share to
    //    XDG_DATA_DIRS.
    //
    // 2. applicationDirPath() is NOT $APPDIR/usr/bin inside the AppImage. The
    //    build puts a COPY of the binary at $APPDIR/AppRun and that is what
    //    runs, so /proc/self/exe -- and therefore applicationDirPath() -- is the
    //    mount ROOT (/tmp/.mount_XXXXXX). "../share/..." resolved to
    //    /tmp/share/... Hence the "/usr/share/..." candidate, relative to root.
    QString script = QStandardPaths::locate(
        QStandardPaths::GenericDataLocation,
        QStringLiteral("process-lasso-qt/install-helper.sh"));

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList relative = {
        QStringLiteral("/usr/share/process-lasso-qt/install-helper.sh"), // AppImage: appDir is the mount root
        QStringLiteral("/../share/process-lasso-qt/install-helper.sh"),  // <prefix>/bin
        QStringLiteral("/../packaging/install-helper.sh"),               // <repo>/build
        QStringLiteral("/packaging/install-helper.sh"),                  // repo root
    };
    QStringList tried;
    for (const auto &rel : relative) {
        const QString candidate = QFileInfo(appDir + rel).absoluteFilePath();
        tried << candidate;
        if (script.isEmpty() && QFile::exists(candidate)) script = candidate;
    }

    if (script.isEmpty()) {
        return {false, QStringLiteral("install-helper.sh not found.\n\nLooked in:\n  %1\n  %2")
                           .arg(QStandardPaths::standardLocations(
                                    QStandardPaths::GenericDataLocation)
                                    .join(QStringLiteral("/process-lasso-qt\n  ")),
                                tried.join(QStringLiteral("\n  ")))};
    }

    // The helper binary the script will install. Same layout problem as the
    // script itself: inside the AppImage, applicationDirPath() is the mount root.
    const QStringList helperRel = {
        QStringLiteral("/usr/bin/process-lasso-helper"),   // AppImage mount root
        QStringLiteral("/process-lasso-helper"),           // <prefix>/bin, or a build dir
        QStringLiteral("/../bin/process-lasso-helper"),
    };
    QString helperSrc;
    QStringList helperTried;
    for (const auto &rel : helperRel) {
        const QString candidate = QFileInfo(appDir + rel).absoluteFilePath();
        helperTried << candidate;
        if (helperSrc.isEmpty() && QFile::exists(candidate)) helperSrc = candidate;
    }
    if (helperSrc.isEmpty())
        return {false, QStringLiteral("process-lasso-helper binary not found.\n\nLooked in:\n  %1")
                           .arg(helperTried.join(QStringLiteral("\n  ")))};

    // Copy both out of the AppImage before handing anything to pkexec.
    //
    // An AppImage is a FUSE mount owned by the invoking user, and FUSE refuses
    // access to every other uid — including root — unless /etc/fuse.conf enables
    // user_allow_other, which is off by default and not something an application
    // may assume. So "pkexec bash /tmp/.mount_XXXX/…/install-helper.sh" fails with
    // a bare "Permission denied" even though the caller authenticated correctly:
    // root genuinely cannot read a path inside that mount.
    //
    // Staging is laid out like an install prefix (<tmp>/share/process-lasso-qt/
    // and <tmp>/bin/) so the script's own "$PREFIX/bin" lookup resolves without
    // it needing to know it is being run from a copy.
    QTemporaryDir stage;
    if (!stage.isValid())
        return {false, QStringLiteral("Could not create a staging directory:\n") +
                       stage.errorString()};

    const QString stagedShare  = stage.filePath(QStringLiteral("share/process-lasso-qt"));
    const QString stagedBin    = stage.filePath(QStringLiteral("bin"));
    if (!QDir().mkpath(stagedShare) || !QDir().mkpath(stagedBin))
        return {false, QStringLiteral("Could not prepare the staging directory.")};

    const QString stagedScript = stagedShare + QStringLiteral("/install-helper.sh");
    const QString stagedHelper = stagedBin   + QStringLiteral("/process-lasso-helper");
    if (!QFile::copy(script, stagedScript))
        return {false, QStringLiteral("Could not stage install-helper.sh from:\n") + script};
    if (!QFile::copy(helperSrc, stagedHelper))
        return {false, QStringLiteral("Could not stage the helper binary from:\n") + helperSrc};

    const auto rx = QFile::ReadOwner  | QFile::WriteOwner | QFile::ExeOwner
                  | QFile::ReadGroup  | QFile::ExeGroup
                  | QFile::ReadOther  | QFile::ExeOther;
    QFile::setPermissions(stagedScript, rx);
    QFile::setPermissions(stagedHelper, rx);

    QProcess p;
    p.start(QStringLiteral("pkexec"),
            QStringList{QStringLiteral("bash"), stagedScript});
    if (!p.waitForFinished(120000))
        return {false, QStringLiteral("pkexec timed out.")};
    if (p.exitCode() == 0)
        return {true, QStringLiteral("Helper and sudoers rule installed.")};
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return {false, QStringLiteral("Install failed:\n") +
                   (err.isEmpty() ? QStringLiteral("pkexec exited with code %1 "
                                                   "(cancelled?)").arg(p.exitCode())
                                  : err)};
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

bool setAffinityViaHelper(int pid, const QString &cpulist)
{
    auto [ok, msg] = runHelper({QStringLiteral("set-affinity"), cpulist,
                                QString::number(pid)});
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
