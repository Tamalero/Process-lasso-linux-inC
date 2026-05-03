#include "utils.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Utils {

QSet<int> cpulistToSet(const QString &cpulist)
{
    QSet<int> result;
    if (cpulist.trimmed().isEmpty()) return result;
    for (const auto &part : cpulist.split(',', Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        if (t.contains('-')) {
            const auto r = t.split('-');
            if (r.size() == 2) {
                bool ok1, ok2;
                int lo = r[0].trimmed().toInt(&ok1);
                int hi = r[1].trimmed().toInt(&ok2);
                if (ok1 && ok2)
                    for (int i = lo; i <= hi; ++i) result.insert(i);
            }
        } else {
            bool ok;
            int n = t.toInt(&ok);
            if (ok) result.insert(n);
        }
    }
    return result;
}

QString cpusetToCpulist(const QSet<int> &cpus)
{
    if (cpus.isEmpty()) return {};
    auto sorted = cpus.values();
    std::sort(sorted.begin(), sorted.end());
    QStringList ranges;
    int start = sorted[0], end = sorted[0];
    for (int i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == end + 1) { end = sorted[i]; }
        else {
            ranges << (start == end ? QString::number(start)
                                    : QStringLiteral("%1-%2").arg(start).arg(end));
            start = end = sorted[i];
        }
    }
    ranges << (start == end ? QString::number(start)
                            : QStringLiteral("%1-%2").arg(start).arg(end));
    return ranges.join(',');
}

QList<int> getTids(int pid)
{
    QList<int> tids;
    const QDir taskDir(QStringLiteral("/proc/%1/task").arg(pid));
    if (!taskDir.exists()) return {pid};
    for (const auto &entry : taskDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok;
        int tid = entry.toInt(&ok);
        if (ok) tids.append(tid);
    }
    return tids.isEmpty() ? QList<int>{pid} : tids;
}

bool setAffinity(int pid, const QString &cpulist)
{
    const auto cpus = cpulistToSet(cpulist);
    if (cpus.isEmpty()) return false;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int cpu : cpus) CPU_SET(cpu, &mask);
    const auto tids = getTids(pid);
    bool anyOk = false;
    for (int tid : tids)
        if (sched_setaffinity(tid, sizeof(mask), &mask) == 0) anyOk = true;
    return anyOk;
}

QString getAffinityStr(int pid)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(pid, sizeof(mask), &mask) != 0) return {};
    QSet<int> cpus;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &mask)) cpus.insert(i);
    return cpusetToCpulist(cpus);
}

bool setNice(int pid, int nice)
{
    return setpriority(PRIO_PROCESS, (unsigned)pid, nice) == 0;
}

bool setIoNice(int pid, int ioclass, int iolevel)
{
    // IOPRIO_PRIO_VALUE(class, data) = (class << 13) | data
    int prio = (ioclass << 13) | (iolevel & 0x1fff);
    return syscall(SYS_ioprio_set, 1 /*IOPRIO_WHO_PROCESS*/, pid, prio) == 0;
}

QSet<int> getOnlineCpus()
{
    QFile f(QStringLiteral("/sys/devices/system/cpu/online"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return cpulistToSet(QString(f.readAll()).trimmed());
}

int getCpuCount()
{
    QFile f(QStringLiteral("/sys/devices/system/cpu/present"));
    if (f.open(QIODevice::ReadOnly)) {
        const auto cpus = cpulistToSet(QString(f.readAll()).trimmed());
        if (!cpus.isEmpty()) return *std::max_element(cpus.cbegin(), cpus.cend()) + 1;
    }
    return (int)sysconf(_SC_NPROCESSORS_CONF);
}

bool validateCpulist(const QString &cpulist)
{
    if (cpulist.trimmed().isEmpty()) return false;
    const int maxCpu = getCpuCount() - 1;
    const auto cpus = cpulistToSet(cpulist);
    if (cpus.isEmpty()) return false;
    for (int cpu : cpus)
        if (cpu < 0 || cpu > maxCpu) return false;
    return true;
}

QString resolveName(const QString &comm, const QStringList &cmdline)
{
    if (!cmdline.isEmpty()) {
        const QString &arg0 = cmdline[0];
        if (arg0.contains('\\') && arg0.toLower().endsWith(QLatin1String(".exe"))) {
            const QString basename = arg0.split('/').last().split('\\').last();
            if (!basename.isEmpty()) return basename;
        }
        if (comm.length() == 15) {
            const QString basename = QFileInfo(arg0).fileName();
            if (!basename.isEmpty() && basename.length() > 15)
                return basename;
        }
    }
    return comm;
}

} // namespace Utils
