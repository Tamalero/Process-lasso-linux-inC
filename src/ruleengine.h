#pragma once
#include <QHash>
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

    bool matches(const QString &procName) const;

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
    void allowHelperForSession(const QString &ruleId) { m_sessionHelper.insert(ruleId); }

    void loadRules(const QJsonArray &arr);
    QJsonArray toJsonArray() const;

    const QList<Rule> &rules() const { return m_rules; }
    void addRule(const Rule &rule);
    void removeRule(const QString &ruleId);
    void updateRule(const Rule &rule);

    // Returns list of action strings for each applied action; empty = no rule matched.
    QStringList applyToProcess(int pid, const QString &procName);

    // Attributes that can never take effect because an earlier enabled rule with
    // the SAME pattern and match type already claims them. ruleId → field names.
    // Only the exactly-duplicated case is reported; general pattern overlap is
    // undecidable, and applyToProcess() handles that safely anyway.
    QHash<QString, QStringList> shadowedAttributes() const;

    // Returns true if any enabled rule with pbExempt=true matches procName.
    bool isPbExempt(const QString &procName) const;

private:
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
    QSet<QString> m_sessionHelper;     // ruleId — allowed for this run only

    void log(const QString &msg);
};
