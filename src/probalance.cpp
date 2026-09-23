#include "probalance.h"
#include "utils.h"
#include <QJsonArray>
#include <algorithm>

ProBalance::ProBalance(const QJsonObject &cfg, LogCb logCb)
    : m_cfg(cfg), m_logCb(std::move(logCb))
{}

void ProBalance::updateConfig(const QJsonObject &cfg) { m_cfg = cfg; }

void ProBalance::log(const QString &msg) { if (m_logCb) m_logCb(msg); }

bool ProBalance::nameMatches(const QString &name, const QString &pattern)
{
    // An empty pattern would be a substring of everything — an empty row in the
    // exempt list must not exempt the entire machine.
    if (pattern.isEmpty()) return false;
    return name.contains(pattern, Qt::CaseInsensitive);
}

bool ProBalance::nameMatchesAny(const QString &name, const QStringList &patterns)
{
    for (const auto &pat : patterns)
        if (nameMatches(name, pat)) return true;
    return false;
}

bool ProBalance::toggleExemptPattern(QStringList &patterns, const QString &name,
                                     bool exempt, QStringList &removed)
{
    if (name.isEmpty()) return false;

    if (exempt) {
        if (nameMatchesAny(name, patterns)) return false;  // already covered
        patterns << name;
        return true;
    }

    QStringList kept;
    for (const auto &pat : patterns) {
        if (nameMatches(name, pat)) removed << pat;
        else                        kept    << pat;
    }
    if (removed.isEmpty()) return false;
    patterns = kept;
    return true;
}

bool ProBalance::isExempt(const QString &name) const
{
    const auto patterns = m_cfg[QStringLiteral("exempt_patterns")].toArray();
    for (const auto &v : patterns)
        if (nameMatches(name, v.toString())) return true;
    return false;
}

void ProBalance::tick(const QList<ProcessInfo> &snapshot, double tickSeconds,
                      const QSet<int> &exemptPids)
{
    if (!m_cfg[QStringLiteral("enabled")].toBool(true)) return;

    const double threshold    = m_cfg[QStringLiteral("cpu_threshold_percent")].toDouble(85.0);
    const double consecThresh = m_cfg[QStringLiteral("consecutive_seconds")].toDouble(3.0);
    const int    adjustment   = m_cfg[QStringLiteral("nice_adjustment")].toInt(10);
    const int    niceFloor    = m_cfg[QStringLiteral("nice_floor")].toInt(15);
    const double restoreThresh= m_cfg[QStringLiteral("restore_threshold_percent")].toDouble(40.0);
    const double restoreHyst  = m_cfg[QStringLiteral("restore_hysteresis_seconds")].toDouble(5.0);

    QSet<int> alivePids;
    for (const auto &p : snapshot) alivePids.insert(p.pid);

    // Remove dead processes
    for (auto it = m_states.begin(); it != m_states.end(); ) {
        if (!alivePids.contains(it.key())) it = m_states.erase(it);
        else ++it;
    }

    for (const auto &proc : snapshot) {
        if (isExempt(proc.name) || exemptPids.contains(proc.pid)) {
            // A process can become exempt *while* it is throttled. Just dropping
            // it out of the loop would strand it at the throttled nice value
            // forever — which is exactly what "exempting it did nothing" looks
            // like from the Processes tab. Undo the throttle first, then forget
            // the process entirely.
            auto it = m_states.find(proc.pid);
            if (it != m_states.end()) {
                if (it->state == State::Throttled) {
                    if (Utils::setNice(proc.pid, it->originalNice))
                        log(QStringLiteral("[ProBalance] RESTORE %1(%2) now exempt, nice %3→%4")
                            .arg(proc.name).arg(proc.pid)
                            .arg(proc.nice).arg(it->originalNice));
                    else
                        log(QStringLiteral("[ProBalance] %1(%2) is exempt but nice %3 could not be restored")
                            .arg(proc.name).arg(proc.pid).arg(proc.nice));
                }
                m_states.erase(it);
            }
            continue;
        }

        if (!m_states.contains(proc.pid))
            m_states[proc.pid] = ProcState{ State::Normal, 0, 0, proc.nice, 0 };

        auto &state = m_states[proc.pid];

        if (state.state == State::Normal) {
            if (proc.cpuPercent > threshold) {
                state.consecutiveHigh += tickSeconds;
                if (state.consecutiveHigh >= consecThresh) {
                    const int newNice = std::min(proc.nice + adjustment, niceFloor);
                    state.originalNice = proc.nice;
                    if (Utils::setNice(proc.pid, newNice)) {
                        state.state         = State::Throttled;
                        state.throttleNice  = newNice;
                        state.consecutiveHigh = 0.0;
                        state.consecutiveLow  = 0.0;
                        log(QStringLiteral("[ProBalance] THROTTLE %1(%2) cpu=%3% nice %4→%5")
                            .arg(proc.name).arg(proc.pid)
                            .arg(proc.cpuPercent, 0, 'f', 1)
                            .arg(proc.nice).arg(newNice));
                    }
                }
            } else {
                state.consecutiveHigh = std::max(0.0, state.consecutiveHigh - tickSeconds);
            }
        } else { // Throttled
            if (proc.cpuPercent < restoreThresh) {
                state.consecutiveLow += tickSeconds;
                if (state.consecutiveLow >= restoreHyst) {
                    if (Utils::setNice(proc.pid, state.originalNice)) {
                        log(QStringLiteral("[ProBalance] RESTORE %1(%2) cpu=%3% nice %4→%5")
                            .arg(proc.name).arg(proc.pid)
                            .arg(proc.cpuPercent, 0, 'f', 1)
                            .arg(proc.nice).arg(state.originalNice));
                    }
                    state.state           = State::Normal;
                    state.consecutiveHigh = 0.0;
                    state.consecutiveLow  = 0.0;
                    state.throttleNice    = 0;
                }
            } else {
                state.consecutiveLow = 0.0;
            }
        }
    }
}

QSet<int> ProBalance::throttledPids() const
{
    QSet<int> result;
    for (auto it = m_states.constBegin(); it != m_states.constEnd(); ++it)
        if (it.value().state == State::Throttled) result.insert(it.key());
    return result;
}
