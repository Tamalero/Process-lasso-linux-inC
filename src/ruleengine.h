#pragma once
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <optional>
#include <functional>

struct Rule {
    QString  ruleId     = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString  name;
    QString  pattern;
    QString  matchType  = QStringLiteral("contains"); // "contains"|"exact"|"regex"
    // What the pattern is tested against. "name" is the process name (the
    // default, and what every rule before 1.5.0 used); "cmdline" is the full
    // command line, which is the only way to tell apart several processes that
    // share a name — a dozen python3.13 interpreters, say.
    QString  matchTarget = QStringLiteral("name");    // "name"|"cmdline"
    std::optional<QString> affinity;
    std::optional<int>     nice;
    std::optional<int>     ioniceClass;
    std::optional<int>     ioniceLevel;
    std::optional<bool>    pbExempt;
    // Opt-in, per rule: may this rule use the privileged helper when the target
    // belongs to another user? Never defaulted on — escalation is the user's
    // decision, made once, in a prompt that names the process.
    std::optional<bool>    allowHelper;
    bool     enabled    = true;

    // cmdline may be empty; a "cmdline" rule simply will not match then.
    bool matches(const QString &procName, const QString &cmdline = QString()) const;

    QJsonObject toJson() const;
    static Rule fromJson(const QJsonObject &obj);
};

class RuleEngine {
public:
    using LogCb = std::function<void(const QString &)>;

    void setLogCallback(LogCb cb) { m_logCb = std::move(cb); }

    // Fired at most once per rule per session when a rule needs root to apply
    // affinity. Runs on the monitor thread — the receiver must hop to the GUI
    // thread before showing anything.
    using EscalationCb = std::function<void(QString ruleId, QString ruleName,
                                            int pid, QString procName,
                                            QString cpulist)>;
    void setEscalationCallback(EscalationCb cb) { m_escalationCb = std::move(cb); }
    // "Yes, but only for this run" — the answer is not written to the rule.
    void allowHelperForSession(const QString &ruleId);

    // ── Threading ───────────────────────────────────────────────────────────
    // The rule list is EDITED on the GUI thread (Rules tab, Processes tab,
    // escalation prompt) and READ on the monitor thread twice a second. Every
    // mutator below takes m_rulesMux; the monitor-thread readers
    // (applyToProcess, isPbExempt, matchesAny) copy the list under it and
    // iterate the copy — QList is implicitly shared, so that copy is a refcount
    // bump and a GUI edit detaches instead of mutating under the reader.
    // rules() returns a reference WITHOUT locking: GUI thread only, which is
    // safe because the GUI thread is the only writer.
    // Everything else in this class (the dedupe sets) is monitor-thread only.
    void loadRules(const QJsonArray &arr);
    QJsonArray toJsonArray() const;

    const QList<Rule> &rules() const { return m_rules; }  // GUI thread only
    void addRule(const Rule &rule);
    void removeRule(const QString &ruleId);
    void updateRule(const Rule &rule);

    // Returns list of action strings for each applied action; empty = no rule matched.
    QStringList applyToProcess(int pid, const QString &procName, const QString &cmdline = QString());

    // Attributes a rule is currently FAILING to apply at runtime, as
    // ruleId → { "affinity"|"nice"|"ionice" → human reason }. The Rules tab
    // marks these, because a rule that cannot apply its setting otherwise looks
    // identical to one that is working.
    //
    // Guarded by its own mutex: written on the monitor thread during
    // applyToProcess(), read on the GUI thread when the table repaints.
    QHash<QString, QHash<QString, QString>> ruleFailures() const;
    // Bumped whenever that map changes, so the GUI can repaint only when there
    // is something new to show instead of rebuilding the table every snapshot.
    quint64 failureGeneration() const;

    // Attributes that can never take effect because an earlier enabled rule with
    // the SAME pattern and match type already claims them. ruleId → field names.
    // Only the exactly-duplicated case is reported; general pattern overlap is
    // undecidable, and applyToProcess() handles that safely anyway.
    QHash<QString, QStringList> shadowedAttributes() const;

    // Returns true if any enabled rule with pbExempt=true matches procName.
    bool isPbExempt(const QString &procName, const QString &cmdline = QString()) const;
    // Does ANY enabled rule match? Use this, not "applyToProcess() returned
    // nothing", to decide whether a default applies: a matching rule whose value
    // is already correct also returns nothing.
    bool matchesAny(const QString &procName, const QString &cmdline = QString()) const;

private:
    mutable QMutex m_rulesMux;          // guards m_rules and m_sessionHelper
    QList<Rule> m_rules;
    LogCb       m_logCb;
    // ruleId → signature of the last "affinity not applied" warning, so a
    // failing rule reports once per parking change instead of every 500 ms.
    QHash<QString, QString> m_affinityWarned;
    // "ruleId|pid" for permission failures already reported. A rule targeting
    // another user's process fails on EVERY pass, so without this it would be a
    // log flood of its own — the exact failure mode fixed in 1.4.1 and 1.4.3.
    QSet<QString> m_permWarned;
    EscalationCb  m_escalationCb;
    QSet<QString> m_escalationAsked;   // ruleId — asked once per session, answer or not
    QSet<QString> m_sessionHelper;     // ruleId — allowed for this run only; m_rulesMux

    // Consistent copy for a monitor-thread pass.
    void snapshot(QList<Rule> &rules, QSet<QString> *sessionHelper = nullptr) const;

    void log(const QString &msg);
    void warnOnce(const Rule &rule, int pid, const QString &procName,
                  const QString &what, int err, const QString &attr);
    void noteFailure(const QString &ruleId, const QString &attr, const QString &reason);
    void clearFailure(const QString &ruleId, const QString &attr);

    mutable QMutex m_failMux;
    QHash<QString, QHash<QString, QString>> m_ruleFailures;
    quint64 m_failGen = 0;
};
