#pragma once
#include <QList>
#include <QJsonObject>
#include <QString>
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
    bool     enabled    = true;

    bool matches(const QString &procName) const;

    QJsonObject toJson() const;
    static Rule fromJson(const QJsonObject &obj);
};

class RuleEngine {
public:
    using LogCb = std::function<void(const QString &)>;

    void setLogCallback(LogCb cb) { m_logCb = std::move(cb); }

    void loadRules(const QJsonArray &arr);
    QJsonArray toJsonArray() const;

    const QList<Rule> &rules() const { return m_rules; }
    void addRule(const Rule &rule);
    void removeRule(const QString &ruleId);
    void updateRule(const Rule &rule);

    // Returns list of action strings for each applied action; empty = no rule matched.
    QStringList applyToProcess(int pid, const QString &procName);

    // Returns true if any enabled rule with pbExempt=true matches procName.
    bool isPbExempt(const QString &procName) const;

private:
    QList<Rule> m_rules;
    LogCb       m_logCb;

    void log(const QString &msg);
};
