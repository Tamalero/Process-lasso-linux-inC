#include "probalance.h"
#include "utils.h"
#include <QJsonArray>
#include <algorithm>

ProBalance::ProBalance(const QJsonObject &cfg, LogCb logCb)
    : m_cfg(cfg), m_logCb(std::move(logCb))
{}

void ProBalance::updateConfig(const QJsonObject &cfg) { m_cfg = cfg; }

void ProBalance::log(const QString &msg) { if (m_logCb) m_logCb(msg); }

bool ProBalance::isExempt(const QString &name) const
{
    const auto patterns = m_cfg[QStringLiteral("exempt_patterns")].toArray();
    const QString lower = name.toLower();
    for (const auto &v : patterns)
        if (lower.contains(v.toString().toLower())) return true;
    return false;
}

void ProBalance::tick(const QList<ProcessInfo> &snapshot, double tickSeconds)
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
        if (isExempt(proc.name)) continue;

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
