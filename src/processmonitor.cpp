#include "processmonitor.h"
#include "cpupark.h"
#include "utils.h"
#include "verbose.h"
#include <QDir>
#include <QFile>
#include <QThread>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>

using namespace std::chrono;

static qint64 nowNs()
{
    return (qint64)duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static long HZ = 100; // overridden in constructor

ProcessMonitor::ProcessMonitor(RuleEngine *re, ProBalance *pb,
                               const QJsonObject &cfg, QObject *parent)
    : QThread(parent), m_ruleEngine(re), m_proBalance(pb), m_config(cfg)
{
    HZ = sysconf(_SC_CLK_TCK);
    re->setLogCallback([this](const QString &m){ emitLog(m); });
    pb->setLogCallback([this](const QString &m){ emitLog(m); });
}

void ProcessMonitor::emitLog(const QString &msg) { emit logMessage(msg); }

void ProcessMonitor::stop() { m_stop = true; }

void ProcessMonitor::updateConfig(const QJsonObject &cfg)
{
    QMutexLocker lk(&m_configMux);
    m_config = cfg;
    m_proBalance->updateConfig(cfg[QStringLiteral("probalance")].toObject());
}

QString ProcessMonitor::defaultAffinity() const
{
    QMutexLocker lk(&m_configMux);
    const auto v = m_config[QStringLiteral("cpu")].toObject()[QStringLiteral("default_affinity")];
    return v.isString() ? v.toString() : QString{};
}

void ProcessMonitor::captureOriginal(int pid)
{
    if (m_originalAffinities.contains(pid)) return;
    cpu_set_t mask; CPU_ZERO(&mask);
    if (sched_getaffinity(pid, sizeof(mask), &mask) == 0) {
        QSet<int> s;
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &mask)) s.insert(i);
        m_originalAffinities[pid] = s;
    }
}

void ProcessMonitor::applyNewPid(const ProcessInfo &info)
{
    captureOriginal(info.pid);
    const auto actions = m_ruleEngine->applyToProcess(info.pid, info.name);
    if (!actions.isEmpty()) {
        if (m_gamingMode && m_gamingNice && !m_gamingNiced.contains(info.pid)) {
            if (CpuPark::setProcessNiceViaHelper(info.pid, -1)) {
                m_gamingNiced[info.pid] = info.nice;
                emitLog(QStringLiteral("[Gaming Mode] nice -1 → %1(%2)")
                    .arg(info.name).arg(info.pid));
            }
        }
    } else {
        const QString def = defaultAffinity();
        if (!def.isEmpty() && Utils::setAffinity(info.pid, def))
            emitLog(QStringLiteral("[Default] affinity=%1 → %2(%3)")
                .arg(def, info.name).arg(info.pid));
    }
}

void ProcessMonitor::restoreGamingNices()
{
    int count = 0;
    for (auto it = m_gamingNiced.constBegin(); it != m_gamingNiced.constEnd(); ++it) {
        if (CpuPark::setProcessNiceViaHelper(it.key(), it.value())) ++count;
    }
    m_gamingNiced.clear();
    emitLog(QStringLiteral("[Gaming Mode] Restored nice for %1 processes.").arg(count));
}

void ProcessMonitor::reapplyAllDefaults()
{
    const QString def = defaultAffinity();
    if (def.isEmpty()) return;
    for (int pid : std::as_const(m_knownPids)) {
        const QString comm = readComm(pid);
        if (comm.isEmpty()) continue;
        const auto cmdline = readCmdline(pid);
        const QString name = Utils::resolveName(comm, cmdline);
        const auto actions = m_ruleEngine->applyToProcess(pid, name);
        if (actions.isEmpty() && Utils::setAffinity(pid, def))
            emitLog(QStringLiteral("[Default] affinity=%1 → %2(%3)").arg(def, name).arg(pid));
    }
}

void ProcessMonitor::resetAllAffinities()
{
    const int total = Utils::getCpuCount();
    QSet<int> allCpus;
    for (int i = 0; i < total; ++i) allCpus.insert(i);
    int count = 0;
    for (auto it = m_originalAffinities.constBegin(); it != m_originalAffinities.constEnd(); ++it) {
        const auto &mask = it.value().isEmpty() ? allCpus : it.value();
        cpu_set_t cs; CPU_ZERO(&cs);
        for (int c : mask) CPU_SET(c, &cs);
        if (sched_setaffinity(it.key(), sizeof(cs), &cs) == 0) {
            for (int tid : Utils::getTids(it.key()))
                sched_setaffinity(tid, sizeof(cs), &cs);
            ++count;
        }
    }
    m_originalAffinities.clear();
    emitLog(QStringLiteral("[Reset] Restored affinity on %1 processes.").arg(count));
}

void ProcessMonitor::setGamingMode(bool active, bool elevateNice)
{
    m_gamingMode = active;
    m_gamingNice = elevateNice;
    if (!active && !m_gamingNiced.isEmpty()) restoreGamingNices();
}

void ProcessMonitor::setManualAffinityOverride(int pid, double durationSeconds)
{
    m_manualOverrides[pid] = (double)nowNs() / 1e9 + durationSeconds;
}

void ProcessMonitor::setPbExempt(int pid, bool exempt)
{
    QMutexLocker lk(&m_configMux);
    if (exempt) m_pbManualExempt.insert(pid);
    else        m_pbManualExempt.remove(pid);
}

QSet<int> ProcessMonitor::pbManualExempt() const
{
    QMutexLocker lk(&m_configMux);
    return m_pbManualExempt;
}

// ── /proc readers ──────────────────────────────────────────────────────────────

bool ProcessMonitor::readProcStat(int pid, long long &utime, long long &stime,
                                   int &nice, qint64 &rss)
{
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    const int lastParen = data.lastIndexOf(')');
    if (lastParen < 0) return false;
    const auto fields = data.mid(lastParen + 2).split(' ');
    // After ')': [0]=state [1]=ppid … [11]=utime [12]=stime [16]=nice [21]=rss(pages)
    if (fields.size() < 22) return false;
    bool ok1, ok2, ok3, ok4;
    utime = fields[11].toLongLong(&ok1);
    stime = fields[12].toLongLong(&ok2);
    nice  = fields[16].toInt(&ok3);
    const long long rssPages = fields[21].toLongLong(&ok4);
    rss   = rssPages * sysconf(_SC_PAGE_SIZE);
    return ok1 && ok2 && ok3 && ok4;
}

QString ProcessMonitor::readComm(int pid)
{
    QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

QStringList ProcessMonitor::readCmdline(int pid)
{
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    QStringList parts;
    for (const auto &part : raw.split('\0'))
        if (!part.isEmpty()) parts << QString::fromLocal8Bit(part);
    return parts;
}

QString ProcessMonitor::readAffinityStr(int pid)
{
    cpu_set_t mask; CPU_ZERO(&mask);
    if (sched_getaffinity(pid, sizeof(mask), &mask) != 0) return {};
    QSet<int> cpus;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &mask)) cpus.insert(i);
    return Utils::cpusetToCpulist(cpus);
}

QString ProcessMonitor::readIoNice(int pid)
{
    const int ioprio = (int)syscall(SYS_ioprio_get, 1 /*IOPRIO_WHO_PROCESS*/, pid);
    if (ioprio < 0) return {};
    return QStringLiteral("%1/%2").arg(ioprio >> 13).arg(ioprio & 0x1fff);
}

// ── Per-CPU system usage from /proc/stat ──────────────────────────────────────

QList<double> ProcessMonitor::readPercpuUsage()
{
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    // /proc virtual files report size 0, so QFile::atEnd() returns true
    // immediately. Read everything at once then split on newlines.
    const QByteArray data = f.readAll();
    QList<double> result;
    for (const QByteArray &line : data.split('\n')) {
        if (!line.startsWith("cpu") || line.startsWith("cpu ")) continue;
        const auto fields = line.split(' ');
        if (fields.size() < 8) continue;
        bool ok;
        const int cpuIdx = fields[0].mid(3).toInt(&ok);
        if (!ok) continue;
        long long user  = fields[1].toLongLong();
        long long nice_ = fields[2].toLongLong();
        long long sys   = fields[3].toLongLong();
        long long idle  = fields[4].toLongLong();
        long long iowait= fields[5].toLongLong();
        long long irq   = fields[6].toLongLong();
        long long sirq  = fields[7].toLongLong();
        long long total = user + nice_ + sys + idle + iowait + irq + sirq;
        long long idleTotal = idle + iowait;
        auto &prev = m_sysCpuPrev[cpuIdx];
        double pct = 0.0;
        if (prev.total > 0) {
            long long dt = total - prev.total;
            long long di = idleTotal - prev.idle;
            pct = dt > 0 ? 100.0 * (1.0 - (double)di / dt) : 0.0;
        }
        prev.total = total;
        prev.idle  = idleTotal;
        while (result.size() <= cpuIdx) result.append(0.0);
        result[cpuIdx] = pct;
    }
    return result;
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void ProcessMonitor::run()
{
    const long long tickMs = 100;

    auto cfgCopy = [&]{
        QMutexLocker lk(&m_configMux);
        return m_config;
    };

    double lastEnforce   = 0.0;
    double lastProbal    = 0.0;
    double lastSnapshot  = 0.0;
    double lastPbTick    = (double)nowNs() / 1e9;

    QList<ProcessInfo> snapshot;

    while (!m_stop) {
        try {
            const double now = (double)nowNs() / 1e9;
            const auto   cfg = cfgCopy();
            const double enforceInterval = cfg[QStringLiteral("monitor")]
                .toObject()[QStringLiteral("rule_enforce_interval_ms")].toDouble(500) / 1000.0;
            const double snapshotInterval = cfg[QStringLiteral("monitor")]
                .toObject()[QStringLiteral("display_refresh_interval_ms")].toDouble(2000) / 1000.0;

            // ── Collect snapshot ──────────────────────────────────────────────
            QList<ProcessInfo> newSnapshot;
            QSet<int> currentPids;

            const QDir procDir(QStringLiteral("/proc"));
            for (const auto &entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                bool ok;
                const int pid = entry.toInt(&ok);
                if (!ok) continue;

                long long utime = 0, stime = 0;
                int nice = 0;
                qint64 rss = 0;
                if (!readProcStat(pid, utime, stime, nice, rss)) continue;

                const QString comm = readComm(pid);
                if (comm.isEmpty()) continue;
                const auto cmdline = readCmdline(pid);
                const QString name = Utils::resolveName(comm, cmdline);

                // CPU%
                auto &cpuState = m_cpuStates[pid];
                const long long ticks = utime + stime;
                const qint64 wallNs = nowNs();
                double cpuPercent = 0.0;
                if (cpuState.prevTicks >= 0) {
                    const long long dt = ticks - cpuState.prevTicks;
                    const double elapsed = (double)(wallNs - cpuState.prevWallNs) / 1e9;
                    if (elapsed > 0 && dt >= 0)
                        cpuPercent = (double)dt / HZ / elapsed * 100.0;
                }
                cpuState.prevTicks  = ticks;
                cpuState.prevWallNs = wallNs;
                cpuState.cpuPercent = cpuPercent;

                ProcessInfo info;
                info.pid        = pid;
                info.name       = name;
                info.cpuPercent = cpuPercent;
                info.memRss     = rss;
                info.nice       = nice;
                info.affinity   = readAffinityStr(pid);
                info.ionice     = readIoNice(pid);
                info.cmdline    = cmdline.join(' ');

                newSnapshot.append(info);
                currentPids.insert(pid);
            }

            // Clean up CPU states for dead processes
            for (auto it = m_cpuStates.begin(); it != m_cpuStates.end(); ) {
                if (!currentPids.contains(it.key())) it = m_cpuStates.erase(it);
                else ++it;
            }

            // New PIDs: apply rules or default affinity
            const QSet<int> newPids = currentPids - m_knownPids;
            if (!newPids.isEmpty()) {
                for (const auto &info : newSnapshot)
                    if (newPids.contains(info.pid)) applyNewPid(info);
            }
            m_knownPids = currentPids;
            snapshot = newSnapshot;

            // ── Rule enforcement ──────────────────────────────────────────────
            if (now - lastEnforce >= enforceInterval) {
                const double nowD = now;
                for (auto it = m_manualOverrides.begin(); it != m_manualOverrides.end(); ) {
                    if (it.value() <= nowD) it = m_manualOverrides.erase(it);
                    else ++it;
                }
                for (const auto &info : snapshot) {
                    if (m_manualOverrides.contains(info.pid)) continue;
                    m_ruleEngine->applyToProcess(info.pid, info.name);
                }
                lastEnforce = now;
            }

            // ── ProBalance ────────────────────────────────────────────────────
            if (now - lastProbal >= 1.0) {
                const double tickSec = now - lastPbTick;
                lastPbTick = now;
                // Merge manual per-pid exemptions with rule-based exemptions.
                QSet<int> pbExempt;
                {
                    QMutexLocker lk(&m_configMux);
                    pbExempt = m_pbManualExempt;
                }
                for (const auto &proc : snapshot)
                    if (m_ruleEngine->isPbExempt(proc.name)) pbExempt.insert(proc.pid);
                m_proBalance->tick(snapshot, tickSec, pbExempt);
                lastProbal = now;
            }

            // ── Emit snapshot ─────────────────────────────────────────────────
            if (now - lastSnapshot >= snapshotInterval) {
                emit processSnapshotReady(snapshot);
                const auto percpu = readPercpuUsage();
                VLOG("monitor: percpu size=%lld", (long long)percpu.size());
                if (!percpu.isEmpty()) {
                    VLOG("monitor: emitting cpuSnapshotReady (first=%.1f%% last=%.1f%%)",
                         percpu.first(), percpu.last());
                    emit cpuSnapshotReady(percpu);
                } else {
                    VLOG("monitor: percpu empty — cpuSnapshotReady NOT emitted");
                }
                // Temperatures are opt-in: skip the hwmon sweep entirely when off.
                if (cfg[QStringLiteral("show_temperatures")].toBool(true)) {
                    const auto sensors = Sensors::read();
                    VLOG("monitor: sensors pkg=%d (%.1f°C) cores=%lld dimms=%lld",
                         (int)sensors.hasPackage, sensors.packageC,
                         (long long)sensors.perCpu.size(),
                         (long long)sensors.memory.size());
                    emit sensorsReady(sensors);
                }
                lastSnapshot = now;
            }

            QThread::msleep(tickMs);
        } catch (...) {
            QThread::msleep(1000);
        }
    }
}
