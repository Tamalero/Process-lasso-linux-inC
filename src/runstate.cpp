#include "runstate.h"
#include "verbose.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <fcntl.h>
#include <unistd.h>

namespace {

QString stateDir()
{
    // XDG state, not config: this is machine-local runtime bookkeeping the user
    // never edits and should not sync between machines.
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/process-lasso");
}

QString currentBootId()
{
    QFile f(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(f.readAll()).trimmed();
}

// Durability of a create/rename lives in the parent directory entry, so the
// directory needs its own fsync after the rename.
void fsyncDir(const QString &dir)
{
    const int fd = ::open(QFile::encodeName(dir).constData(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// Atomic + durable. Both halves matter: /home here is btrfs mounted
// commit=120, so without fsync a write can sit up to two minutes before it
// reaches the disk — useless for a marker whose whole job is to survive a
// power cut.
bool writeState(const QJsonObject &obj)
{
    const QString dir = stateDir();
    QDir().mkpath(dir);

    const QString target = RunState::path();
    const QString tmp    = target + QStringLiteral(".tmp");

    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        if (f.write(data) != data.size()) return false;
        if (!f.flush()) return false;
        if (::fsync(f.handle()) != 0) return false;
    }

    // POSIX rename replaces atomically; QFile::rename refuses an existing
    // target, which would leave a window with no marker at all.
    if (::rename(QFile::encodeName(tmp).constData(),
                 QFile::encodeName(target).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    fsyncDir(dir);
    return true;
}

QJsonObject baseObject(const char *state, int crashCount)
{
    QJsonObject o;
    o[QStringLiteral("state")]       = QString::fromLatin1(state);
    o[QStringLiteral("boot_id")]     = currentBootId();
    o[QStringLiteral("crash_count")] = crashCount;
    o[QStringLiteral("updated")]     =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return o;
}

} // namespace

QString RunState::path()
{
    return stateDir() + QStringLiteral("/runstate.json");
}

RunStateInfo RunState::beginSession()
{
    RunStateInfo info;
    const QString boot = currentBootId();

    QJsonObject prev;
    {
        QFile f(path());
        if (f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) { prev = doc.object(); info.hadPreviousRun = true; }
        }
    }

    if (info.hadPreviousRun) {
        info.previousWasClean =
            prev[QStringLiteral("state")].toString() == QLatin1String("clean");
        info.sameBoot = !boot.isEmpty() &&
            prev[QStringLiteral("boot_id")].toString() == boot;
        info.crashCount = prev[QStringLiteral("crash_count")].toInt(0);
    }
    // No marker at all is a first run or a wiped state dir — not a crash.
    // Treating it as one would make every fresh install look broken.

    if (info.previousWasClean) info.crashCount  = 0;
    else                       info.crashCount += 1;

    info.safeMode = info.crashCount >= SAFE_MODE_THRESHOLD;

    // Re-arm immediately, before the caller applies any config.
    writeState(baseObject("dirty", info.crashCount));

    VLOG("RunState::beginSession: hadPrev=%d clean=%d sameBoot=%d crashes=%d safe=%d",
         (int)info.hadPreviousRun, (int)info.previousWasClean,
         (int)info.sameBoot, info.crashCount, (int)info.safeMode);
    return info;
}

void RunState::markHealthy()
{
    writeState(baseObject("dirty", 0));
    VLOG("RunState::markHealthy: crash counter reset");
}

void RunState::markClean()
{
    writeState(baseObject("clean", 0));
    VLOG("RunState::markClean: shutdown recorded as clean");
}
