#pragma once
#include "processinfo.h"
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <functional>

class ProBalance {
public:
    using LogCb = std::function<void(const QString &)>;

    explicit ProBalance(const QJsonObject &cfg, LogCb logCb = nullptr);

    void updateConfig(const QJsonObject &cfg);
    void setLogCallback(LogCb cb) { m_logCb = std::move(cb); }

    void tick(const QList<ProcessInfo> &snapshot, double tickSeconds,
              const QSet<int> &exemptPids = {});

    QSet<int> throttledPids() const;

    // ── The exempt-list rules, in one place ────────────────────────────────
    // Every ProBalance exempt list (the persisted one, the session one, and the
    // ticks in the Processes tab context menu) matches the same way: a pattern
    // is a case-insensitive *substring* of the process name. These used to be
    // four separate reimplementations; keep them here so they cannot drift.
    static bool nameMatches(const QString &name, const QString &pattern);
    static bool nameMatchesAny(const QString &name, const QStringList &patterns);

    // Adds or removes `name` in an exempt list. Adding is a no-op when some
    // pattern already covers the name. Removing drops *every* pattern that
    // matches it — removing only an exact-name entry would leave a broader
    // pattern in place and the process still exempt, while the UI said it was
    // not. Removed patterns are appended to `removed`.
    // Returns false when the list was left unchanged.
    static bool toggleExemptPattern(QStringList &patterns, const QString &name,
                                    bool exempt, QStringList &removed);

private:
    enum class State { Normal, Throttled };
    struct ProcState {
        State  state         = State::Normal;
        double consecutiveHigh = 0.0;
        double consecutiveLow  = 0.0;
        int    originalNice  = 0;
        int    throttleNice  = 0;
    };

    QJsonObject        m_cfg;
    QHash<int, ProcState> m_states;
    LogCb              m_logCb;

    bool isExempt(const QString &name) const;
    void log(const QString &msg);
};
