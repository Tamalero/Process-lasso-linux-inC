#include "cputopology.h"
#include <QFile>
#include <QDir>
#include <algorithm>
#include <unistd.h>

static CpuTopology s_topoCache;
static bool        s_hasCachedAsymmetric = false;

QSet<int> parseCpulistFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString content = QString(f.readAll()).trimmed();
    if (content.isEmpty()) return {};
    QSet<int> result;
    for (const auto &part : content.split(',', Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        if (t.contains('-')) {
            const auto r = t.split('-');
            if (r.size() == 2) {
                bool ok1, ok2;
                int lo = r[0].toInt(&ok1), hi = r[1].toInt(&ok2);
                if (ok1 && ok2) for (int i = lo; i <= hi; ++i) result.insert(i);
            }
        } else {
            bool ok;
            int n = t.toInt(&ok);
            if (ok) result.insert(n);
        }
    }
    return result;
}

QString formatCpuSet(const QSet<int> &cpus)
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

QSet<int> getOnlineCpuSet()  { return parseCpulistFile(QStringLiteral("/sys/devices/system/cpu/online")); }
QSet<int> getOfflineCpuSet() { return parseCpulistFile(QStringLiteral("/sys/devices/system/cpu/offline")); }

QSet<int> getSmtSiblingsOf(const QSet<int> &cpus)
{
    QHash<int, QList<int>> coreToLogical;
    for (int cpu : cpus) {
        QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/topology/core_id").arg(cpu));
        if (f.open(QIODevice::ReadOnly)) {
            bool ok;
            int coreId = QString(f.readAll()).trimmed().toInt(&ok);
            if (ok) coreToLogical[coreId].append(cpu);
        }
    }
    QSet<int> siblings;
    for (auto &logical : coreToLogical) {
        if (logical.size() >= 2) {
            std::sort(logical.begin(), logical.end());
            for (int i = 1; i < logical.size(); ++i) siblings.insert(logical[i]);
        }
    }
    return siblings;
}

static QSet<int> presentCpus()
{
    auto cpus = parseCpulistFile(QStringLiteral("/sys/devices/system/cpu/present"));
    if (cpus.isEmpty()) {
        const int n = (int)sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < n; ++i) cpus.insert(i);
    }
    return cpus;
}

static CpuTopology detectAmdX3D()
{
    const auto present = presentCpus();
    const auto offline = parseCpulistFile(QStringLiteral("/sys/devices/system/cpu/offline"));
    QHash<int, int> l3; // cpu → KB
    for (int cpu : present) {
        QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/cache/index3/size").arg(cpu));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString raw = QString(f.readAll()).trimmed();
        bool ok; int kb = 0;
        if      (raw.endsWith('K')) kb = raw.left(raw.size()-1).toInt(&ok);
        else if (raw.endsWith('M')) kb = raw.left(raw.size()-1).toInt(&ok) * 1024;
        else                        kb = raw.toInt(&ok);
        if (ok && kb > 0) l3[cpu] = kb;
    }
    if (l3.isEmpty()) return {};
    QSet<int> sizes;
    for (auto v : l3) sizes.insert(v);
    if (sizes.size() <= 1) {
        if (!offline.isEmpty() && !l3.isEmpty()) {
            int onlineKb = *sizes.cbegin();
            QSet<int> onlineSet;
            for (auto it = l3.cbegin(); it != l3.cend(); ++it) onlineSet.insert(it.key());
            return { TopologyKind::AmdX3D, onlineSet, offline,
                     QStringLiteral("AMD X3D detected (other CCD currently parked). "
                                    "Preferred (V-Cache, %1MB L3): CPUs %2. "
                                    "Non-preferred (parked): CPUs %3.")
                         .arg(onlineKb/1024).arg(formatCpuSet(onlineSet)).arg(formatCpuSet(offline)) };
        }
        return {};
    }
    int maxKb = *std::max_element(sizes.cbegin(), sizes.cend());
    int minKb = *std::min_element(sizes.cbegin(), sizes.cend());
    QSet<int> preferred, nonPreferred;
    for (auto it = l3.cbegin(); it != l3.cend(); ++it) {
        if      (it.value() == maxKb) preferred.insert(it.key());
        else if (it.value() == minKb) nonPreferred.insert(it.key());
    }
    return { TopologyKind::AmdX3D, preferred, nonPreferred,
             QStringLiteral("AMD X3D detected. Preferred (V-Cache, %1MB L3): CPUs %2. "
                            "Non-preferred (%3MB L3): CPUs %4.")
                 .arg(maxKb/1024).arg(formatCpuSet(preferred))
                 .arg(minKb/1024).arg(formatCpuSet(nonPreferred)) };
}

static CpuTopology detectIntelHybrid()
{
    const auto present = presentCpus();
    QHash<int, int> maxFreq; // cpu → kHz
    for (int cpu : present) {
        QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/cpuinfo_max_freq").arg(cpu));
        if (!f.open(QIODevice::ReadOnly)) continue;
        bool ok; int freq = QString(f.readAll()).trimmed().toInt(&ok);
        if (ok && freq > 0) maxFreq[cpu] = freq;
    }
    if (maxFreq.isEmpty()) return {};
    QSet<int> freqs;
    for (auto v : maxFreq) freqs.insert(v);
    if (freqs.size() <= 1) return {};
    int maxF = *std::max_element(freqs.cbegin(), freqs.cend());
    int minF = *std::min_element(freqs.cbegin(), freqs.cend());
    int threshold = (int)(maxF * 0.80);
    QSet<int> preferred, nonPreferred;
    for (auto it = maxFreq.cbegin(); it != maxFreq.cend(); ++it) {
        if (it.value() >= threshold) preferred.insert(it.key());
        else                         nonPreferred.insert(it.key());
    }
    return { TopologyKind::IntelHybrid, preferred, nonPreferred,
             QStringLiteral("Intel Hybrid detected. P-cores (%1 GHz max): CPUs %2. "
                            "E-cores (%3 GHz max): CPUs %4.")
                 .arg(maxF/1e6, 0, 'f', 1).arg(formatCpuSet(preferred))
                 .arg(minF/1e6, 0, 'f', 1).arg(formatCpuSet(nonPreferred)) };
}

CpuTopology detectTopology()
{
    auto topo = detectAmdX3D();
    if (topo.hasAsymmetry()) { s_topoCache = topo; s_hasCachedAsymmetric = true; return topo; }
    topo = detectIntelHybrid();
    if (topo.hasAsymmetry()) { s_topoCache = topo; s_hasCachedAsymmetric = true; return topo; }
    if (s_hasCachedAsymmetric) return s_topoCache;
    const auto allCpus = presentCpus();
    return { TopologyKind::Uniform, allCpus, {},
             QStringLiteral("Uniform topology (no asymmetry detected). All CPUs equal.") };
}
