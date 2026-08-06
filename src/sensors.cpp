#include "sensors.h"
#include "verbose.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <unistd.h>

// ── SensorSnapshot ────────────────────────────────────────────────────────────

bool SensorSnapshot::cpuMax(double &out) const
{
    if (hasPackage) { out = packageC; return true; }
    if (perCpu.isEmpty()) return false;
    double m = -1e9;
    for (double v : perCpu) m = std::max(m, v);
    out = m;
    return true;
}

bool SensorSnapshot::memoryMax(double &out) const
{
    if (memory.isEmpty()) return false;
    double m = -1e9;
    for (const auto &r : memory) m = std::max(m, r.celsius);
    out = m;
    return true;
}

// ── sysfs helpers ─────────────────────────────────────────────────────────────

namespace {

// /sys files are virtual: QFile::atEnd() lies about them, so always readAll().
QString readTextFile(const QString &path, bool *ok = nullptr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return {}; }
    if (ok) *ok = true;
    return QString::fromLatin1(f.readAll()).trimmed();
}

int readIntFile(const QString &path, bool *ok)
{
    bool opened = false;
    const QString s = readTextFile(path, &opened);
    if (!opened) { *ok = false; return 0; }
    return s.toInt(ok);
}

enum SourceKind { CpuPackage, CpuCore, Memory };

struct TempSource {
    QString    inputPath;
    QString    label;
    SourceKind kind   = CpuPackage;
    int        coreId = -1;   // CpuCore only
    int        pkgId  = 0;    // CpuPackage / CpuCore
};

QList<TempSource> g_sources;
bool              g_discovered = false;

// logical CPU → (physical package, core id). Built lazily; offline CPUs drop
// out of sysfs, so misses are retried rather than cached as negatives.
QHash<int, QPair<int, int>> g_cpuTopo;

void ensureTopology(int nCpus)
{
    for (int cpu = 0; cpu < nCpus; ++cpu) {
        if (g_cpuTopo.contains(cpu)) continue;
        const QString base =
            QStringLiteral("/sys/devices/system/cpu/cpu%1/topology/").arg(cpu);
        bool okCore = false, okPkg = false;
        const int core = readIntFile(base + QStringLiteral("core_id"), &okCore);
        const int pkg  = readIntFile(base + QStringLiteral("physical_package_id"), &okPkg);
        if (okCore) g_cpuTopo.insert(cpu, qMakePair(okPkg ? pkg : 0, core));
    }
}

// hwmon dirs sort as hwmon0, hwmon1, hwmon10, hwmon2 … — order them numerically
// so DIMM numbering stays stable across runs.
QStringList hwmonDirsInNumericOrder()
{
    const QString base = QStringLiteral("/sys/class/hwmon");
    QStringList names = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });
    QStringList paths;
    for (const auto &n : names) paths << base + '/' + n;
    return paths;
}

// coretemp registers one platform device per socket: /sys/devices/platform/coretemp.N
int packageIdOf(const QString &hwPath)
{
    static const QRegularExpression re(QStringLiteral("coretemp\\.(\\d+)"));
    const auto m = re.match(QFileInfo(hwPath).canonicalFilePath());
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

void discover()
{
    g_sources.clear();
    int dimmIndex = 0;

    for (const QString &hwPath : hwmonDirsInNumericOrder()) {
        const QString hwName = readTextFile(hwPath + QStringLiteral("/name"));
        if (hwName.isEmpty()) continue;

        const bool isIntelCpu = (hwName == QLatin1String("coretemp"));
        const bool isAmdCpu   = (hwName == QLatin1String("k10temp") ||
                                 hwName == QLatin1String("zenpower") ||
                                 hwName == QLatin1String("zenpower3"));
        // DDR5 on-DIMM sensors (spd5118) and the DDR3/DDR4 equivalent (jc42).
        const bool isDimm     = (hwName == QLatin1String("spd5118") ||
                                 hwName == QLatin1String("jc42"));
        if (!isIntelCpu && !isAmdCpu && !isDimm) continue;

        const int pkg = isIntelCpu ? packageIdOf(hwPath) : 0;

        QStringList files = QDir(hwPath).entryList(QDir::Files);
        std::sort(files.begin(), files.end());
        for (const QString &fname : files) {
            if (!fname.startsWith(QLatin1String("temp")) ||
                !fname.endsWith(QLatin1String("_input"))) continue;

            const QString stem = fname.left(fname.size() - 6); // strip "_input"
            // spd5118/jc42 expose no *_label; synthesise one.
            const QString label = readTextFile(hwPath + '/' + stem +
                                               QStringLiteral("_label"));

            TempSource src;
            src.inputPath = hwPath + '/' + fname;
            src.pkgId     = pkg;

            if (isDimm) {
                src.kind  = Memory;
                src.label = label.isEmpty()
                    ? QStringLiteral("DIMM %1").arg(++dimmIndex)
                    : label;
            } else if (label.startsWith(QLatin1String("Core "))) {
                bool ok = false;
                const int coreId = label.mid(5).toInt(&ok);
                if (!ok) continue;
                src.kind   = CpuCore;
                src.coreId = coreId;
                src.label  = label;
            } else if (label.startsWith(QLatin1String("Package id")) ||
                       label == QLatin1String("Tdie") ||
                       label == QLatin1String("Tctl")) {
                src.kind  = CpuPackage;
                src.label = label;
            } else {
                continue; // Tccd*, "Composite", unknown — not surfaced yet
            }
            g_sources.append(src);
        }
    }

    g_discovered = true;
    VLOG("Sensors::discover: %lld sources", (long long)g_sources.size());
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────────

SensorSnapshot Sensors::read()
{
    if (!g_discovered) discover();

    SensorSnapshot snap;
    bool sawTdie   = false;   // Tdie is more accurate than Tctl; prefer it
    bool stalePath = false;

    // Core sensors are reported per (socket, physical core); SMT siblings and
    // multi-socket boards both reuse core ids, so key on the pair.
    QHash<QPair<int, int>, double> byPkgCore;

    for (const TempSource &src : std::as_const(g_sources)) {
        bool ok = false;
        const QString raw = readTextFile(src.inputPath, &ok);
        if (!ok) { stalePath = true; continue; }
        bool numOk = false;
        const double c = raw.toDouble(&numOk) / 1000.0;
        if (!numOk) continue;

        switch (src.kind) {
        case CpuPackage:
            if (!snap.hasPackage || (!sawTdie && src.label == QLatin1String("Tdie"))) {
                snap.hasPackage = true;
                snap.packageC   = c;
                if (src.label == QLatin1String("Tdie")) sawTdie = true;
            }
            break;
        case CpuCore:
            byPkgCore.insert(qMakePair(src.pkgId, src.coreId), c);
            break;
        case Memory:
            snap.memory.append({ src.label, c });
            break;
        }
    }

    // Re-key per-core readings onto logical CPU indices, so both SMT siblings
    // of a physical core report that core's temperature.
    if (!byPkgCore.isEmpty()) {
        const long conf = sysconf(_SC_NPROCESSORS_CONF);
        ensureTopology(conf > 0 ? (int)conf : 0);
        for (auto it = g_cpuTopo.constBegin(); it != g_cpuTopo.constEnd(); ++it) {
            const auto found = byPkgCore.constFind(it.value());
            if (found != byPkgCore.constEnd()) snap.perCpu.insert(it.key(), *found);
        }
    }

    // A sensor vanished (module unloaded, device unplugged) — rediscover next pass.
    if (stalePath) g_discovered = false;

    return snap;
}

bool Sensors::available()
{
    if (!g_discovered) discover();
    return !g_sources.isEmpty();
}
