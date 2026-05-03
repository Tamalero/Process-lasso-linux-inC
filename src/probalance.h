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

    void tick(const QList<ProcessInfo> &snapshot, double tickSeconds);

    QSet<int> throttledPids() const;

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
