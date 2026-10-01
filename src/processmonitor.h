#pragma once
#include "processinfo.h"
#include "ruleengine.h"
#include "probalance.h"
#include "sensors.h"
#include <QThread>
#include <QMutex>
#include <QSet>
#include <QHash>
#include <QJsonObject>
#include <atomic>

class ProcessMonitor : public QThread {
    Q_OBJECT
public:
    ProcessMonitor(RuleEngine *ruleEngine,
                   ProBalance *proBalance,
                   const QJsonObject &config,
                   QObject *parent = nullptr);

    void stop();
    void updateConfig(const QJsonObject &config);
    // The next three are REQUESTS: safe to call from the GUI thread, carried out
    // by run() on the monitor thread at its next loop (≤100 ms). They used to do
    // the work inline on the caller's thread, racing run() over m_knownPids,
    // m_originalAffinities, m_gamingNiced and RuleEngine's dedupe sets.
    void reapplyAllDefaults();
    void resetAllAffinities();
    void setGamingMode(bool active, bool elevateNice);
    // ProBalance's throttled set as of its last tick. ProBalance itself is
    // monitor-thread only; this is a copy published under m_configMux.
    QSet<int> throttledPids() const;
    // Suppress ALL rule enforcement for one pid (affinity, nice and ionice —
    // the enforcement loop skips the whole applyToProcess call). A duration of
    // 0 means indefinite: until the process exits or the override is cleared.
    void setManualOverride(int pid, double durationSeconds = 30.0);
    // ProBalance exemptions that last only for this run — same name-pattern
    // matching as the persisted list, but owned by MainWindow, not config.json.
    void setSessionExemptPatterns(const QStringList &patterns);
    // Force one rule-enforcement pass on the next loop and report the outcome
    // in the Log. Enforcement already runs every rule_enforce_interval_ms, so
    // this exists for FEEDBACK — "did my rule actually do anything?" — not to
    // make it happen more often.
    void reapplyRulesNow();
    // Observe-only: keep monitoring, stop applying anything config-driven.
    void setSafeMode(bool on);

signals:
    void processSnapshotReady(QList<ProcessInfo> snapshot);
    void cpuSnapshotReady(QList<double> percpu);
    void sensorsReady(SensorSnapshot sensors);
    void logMessage(QString msg);
    // A rule needs root to set affinity on someone else's process. Queued to the
    // GUI thread, which asks the user; the engine has already deduped to one
    // per rule per session.
    void affinityEscalationNeeded(QString ruleId, QString ruleName, int pid,
                                  QString procName, QString cpulist);

protected:
    void run() override;

private:
    // Process stats for CPU% tracking
    struct ProcCpuState {
        long long prevTicks  = -1;
        qint64    prevWallNs = 0;
        double    cpuPercent = 0.0;
    };

    // Per-CPU system stats for percpu bars
    struct SysCpuStat {
        long long idle  = 0;
        long long total = 0;
    };

    RuleEngine   *m_ruleEngine;
    ProBalance   *m_proBalance;
    QJsonObject   m_config;
    mutable QMutex m_configMux;
    std::atomic<bool> m_stop{false};    // written by the GUI thread, read by run()
    bool          m_safeMode = false;   // guarded by m_configMux
    // Pending GUI requests, all guarded by m_configMux and consumed by run().
    bool          m_reapplyDefaultsReq = false;
    bool          m_resetAffinitiesReq = false;
    bool          m_gamingReq          = false;
    bool          m_gamingReqActive    = false;
    bool          m_gamingReqNice      = false;
    QSet<int>     m_throttledPub;      // published copy of ProBalance::throttledPids()
    // Set by updateConfig(); consumed by run() on the monitor thread, which is
    // the only thread allowed to write ProBalance's config (it has no mutex).
    bool          m_pbConfigDirty = true;  // guarded by m_configMux
    bool          m_forceEnforce  = false; // guarded by m_configMux
    // Monitor-thread only: refreshed once per loop so captureOriginal() does
    // not re-read /sys for every PID.
    bool          m_cpusParked = false;
    QString       m_defAffinityWarnSig;   // dedupes the "could not apply" log

    // Monitor-thread only. Pruned of exited pids every loop: a recycled pid must
    // not inherit the previous process's "original" mask or nice value.
    QSet<int>               m_knownPids;
    QHash<int, QSet<int>>   m_originalAffinities; // pid → original affinity
    QHash<int, ProcCpuState>m_cpuStates;
    QHash<int, SysCpuStat>  m_sysCpuPrev;

    bool              m_gamingMode      = false;
    bool              m_gamingNice      = false;
    QHash<int, int>   m_gamingNiced;     // pid → original nice

    QHash<int, double> m_manualOverrides;  // pid → expiry monotonic s; guarded by m_configMux
    QStringList        m_pbSessionExempt;  // guarded by m_configMux

    QString defaultAffinity() const;
    // Monitor-thread bodies of the requests above.
    void    doReapplyAllDefaults(const QList<ProcessInfo> &snapshot);
    void    doResetAllAffinities();
    void    applyNewPid(const ProcessInfo &info);
    void    restoreGamingNices();
    void    captureOriginal(int pid);
    void    emitLog(const QString &msg);

    // /proc readers
    static bool readProcStat(int pid, long long &utime, long long &stime,
                              int &nice, qint64 &rss);
    static QString readComm(int pid);
    static QStringList readCmdline(int pid);
    static QString readAffinityStr(int pid);
    static QString readIoNice(int pid);

    // System-wide per-CPU usage
    QList<double> readPercpuUsage();
};
