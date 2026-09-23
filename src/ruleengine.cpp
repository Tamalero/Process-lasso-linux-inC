#include "ruleengine.h"
#include <QSet>
#include "utils.h"
#include "cputopology.h"
#include "verbose.h"
#include <QJsonArray>
#include <QRegularExpression>

// ── Rule ──────────────────────────────────────────────────────────────────────

bool Rule::matches(const QString &procName) const
{
    if (!enabled || pattern.isEmpty()) return false;
    if (matchType == QLatin1String("exact"))
        return procName == pattern;
    if (matchType == QLatin1String("regex")) {
        const QRegularExpression re(pattern);
        return re.isValid() && re.match(procName).hasMatch();
    }
    // "contains" (default)
    return procName.toLower().contains(pattern.toLower());
}

QJsonObject Rule::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("rule_id")]     = ruleId;
    obj[QStringLiteral("name")]        = name;
    obj[QStringLiteral("pattern")]     = pattern;
    obj[QStringLiteral("match_type")]  = matchType;
    obj[QStringLiteral("affinity")]    = affinity ? QJsonValue(*affinity) : QJsonValue::Null;
    obj[QStringLiteral("nice")]        = nice     ? QJsonValue(*nice)     : QJsonValue::Null;
    obj[QStringLiteral("ionice_class")]= ioniceClass ? QJsonValue(*ioniceClass) : QJsonValue::Null;
    obj[QStringLiteral("ionice_level")]= ioniceLevel ? QJsonValue(*ioniceLevel) : QJsonValue::Null;
    obj[QStringLiteral("pb_exempt")]   = pbExempt   ? QJsonValue(*pbExempt)    : QJsonValue::Null;
    obj[QStringLiteral("enabled")]     = enabled;
    return obj;
}

Rule Rule::fromJson(const QJsonObject &obj)
{
    Rule r;
    r.ruleId    = obj[QStringLiteral("rule_id")].toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
    r.name      = obj[QStringLiteral("name")].toString();
    r.pattern   = obj[QStringLiteral("pattern")].toString();
    r.matchType = obj[QStringLiteral("match_type")].toString(QStringLiteral("contains"));
    r.enabled   = obj[QStringLiteral("enabled")].toBool(true);
    const auto aff = obj[QStringLiteral("affinity")];
    if (!aff.isNull() && aff.isString()) r.affinity = aff.toString();
    const auto nice = obj[QStringLiteral("nice")];
    if (!nice.isNull() && nice.isDouble()) r.nice = nice.toInt();
    const auto ioc = obj[QStringLiteral("ionice_class")];
    if (!ioc.isNull() && ioc.isDouble()) r.ioniceClass = ioc.toInt();
    const auto iol = obj[QStringLiteral("ionice_level")];
    if (!iol.isNull() && iol.isDouble()) r.ioniceLevel = iol.toInt();
    const auto pbe = obj[QStringLiteral("pb_exempt")];
    if (!pbe.isNull() && pbe.isBool()) r.pbExempt = pbe.toBool();
    return r;
}

// ── RuleEngine ────────────────────────────────────────────────────────────────

void RuleEngine::log(const QString &msg) { if (m_logCb) m_logCb(msg); }

void RuleEngine::loadRules(const QJsonArray &arr)
{
    m_affinityWarned.clear();
    m_rules.clear();
    for (const auto &v : arr)
        if (v.isObject()) m_rules.append(Rule::fromJson(v.toObject()));
}

QJsonArray RuleEngine::toJsonArray() const
{
    QJsonArray arr;
    for (const auto &r : m_rules) arr.append(r.toJson());
    return arr;
}

void RuleEngine::addRule(const Rule &rule) { m_rules.append(rule); }

void RuleEngine::removeRule(const QString &ruleId)
{
    m_rules.removeIf([&](const Rule &r){ return r.ruleId == ruleId; });
}

void RuleEngine::updateRule(const Rule &rule)
{
    for (auto &r : m_rules) {
        if (r.ruleId == rule.ruleId) { r = rule; return; }
    }
}

QStringList RuleEngine::applyToProcess(int pid, const QString &procName)
{
    QStringList actions;
    // FIRST MATCHING RULE WINS, PER ATTRIBUTE.
    //
    // Nothing stops two enabled rules matching the same process. If both set the
    // same attribute to different values they overwrite each other on every
    // enforcement pass — each one "correcting" the other twice a second, forever,
    // logging as it goes. That is a log flood the 1.4.1 no-op skip cannot damp,
    // because the value really is changing every time.
    //
    // Per attribute, not per rule: one rule setting affinity and another setting
    // nice for the same process is a legitimate combination and still works.
    bool affinityDone = false, niceDone = false, ioniceDone = false;
    for (const auto &rule : m_rules) {
        if (!rule.matches(procName)) continue;
        if (rule.affinity && !affinityDone) {
            // Claimed even if the write below fails, so a losing rule cannot
            // step in on the next pass and restart the fight.
            affinityDone = true;
            // Enforcement runs twice a second over every matching pid. Writing
            // and logging a value that is ALREADY correct turned that into a
            // flood: ~150 browser processes produced ~300 log lines/second into
            // the Log tab's QTextEdit on the GUI thread, plus a
            // sched_setaffinity for every *thread* of every one of them. The
            // GUI thread saturates and the app stops responding to input —
            // which looks exactly like setting affinity being ignored.
            // So: read first, and do nothing at all when nothing needs doing.
            const QSet<int> want = Utils::cpulistToSet(*rule.affinity);
            const QSet<int> have = Utils::cpulistToSet(Utils::getAffinityStr(pid));
            if (!have.isEmpty() && have == want) {
                // Already correct. No syscall, no log line.
            } else if (Utils::setAffinity(pid, *rule.affinity)) {
                const QString msg = QStringLiteral("[Rule:%1] affinity=%2 → %3(%4)")
                    .arg(rule.name, *rule.affinity, procName).arg(pid);
                log(msg); actions << msg;
                m_affinityWarned.remove(rule.ruleId);
            } else {
                // This used to fail silently: the log line lived inside the
                // success branch, so a rule targeting parked CPUs did nothing
                // and said nothing. sched_setaffinity returns EINVAL when every
                // requested CPU is offline, which is exactly what Gaming Mode
                // does. Deduped on (requested, parked) so the enforcement loop
                // does not repeat it twice a second.
                const QSet<int> want    = Utils::cpulistToSet(*rule.affinity);
                const QSet<int> offline = getOfflineCpuSet();
                // Only the parked case is worth telling the user about: it is
                // actionable and cannot interleave with success. Anything else
                // is almost always ESRCH (the process exited between the
                // snapshot and the syscall) — noise, not a problem.
                if (want.isEmpty() || !(want - offline).isEmpty()) {
                    VLOG("rule '%s': affinity '%s' failed for %s (transient)",
                         qPrintable(rule.name), qPrintable(*rule.affinity),
                         qPrintable(procName));
                } else if (m_affinityWarned.value(rule.ruleId) != *rule.affinity) {
                    m_affinityWarned[rule.ruleId] = *rule.affinity;
                    VLOG("rule '%s': affinity '%s' NOT applied to %s — all parked (%s)",
                         qPrintable(rule.name), qPrintable(*rule.affinity),
                         qPrintable(procName), qPrintable(Utils::cpusetToCpulist(offline)));
                    log(QStringLiteral("[Rule:%1] affinity=%2 NOT applied to %3 — "
                                       "every one of those CPUs is parked.")
                            .arg(rule.name, *rule.affinity, procName));
                }
            }
        }
        if (rule.nice && !niceDone) {
            niceDone = true;
            int curNice = 0;
            const bool niceKnown = Utils::getNice(pid, curNice);
            if (niceKnown && curNice == *rule.nice) {
                // Already correct — same reasoning as affinity above.
            } else if (Utils::setNice(pid, *rule.nice)) {
                const QString msg = QStringLiteral("[Rule:%1] nice=%2 → %3(%4)")
                    .arg(rule.name).arg(*rule.nice).arg(procName).arg(pid);
                log(msg); actions << msg;
            }
        }
        if (rule.ioniceClass && !ioniceDone) {
            ioniceDone = true;
            const int level = rule.ioniceLevel.value_or(0);
            int curClass = 0, curLevel = 0;
            const bool ioKnown = Utils::getIoNice(pid, curClass, curLevel);
            if (ioKnown && curClass == *rule.ioniceClass && curLevel == level) {
                // Already correct — same reasoning as affinity above.
            } else if (Utils::setIoNice(pid, *rule.ioniceClass, level)) {
                const QString msg = QStringLiteral("[Rule:%1] ionice class=%2 level=%3 → %4(%5)")
                    .arg(rule.name).arg(*rule.ioniceClass).arg(level).arg(procName).arg(pid);
                log(msg); actions << msg;
            }
        }
    }
    return actions;
}

QHash<QString, QStringList> RuleEngine::shadowedAttributes() const
{
    QHash<QString, QStringList> out;
    QSet<QString> haveAffinity, haveNice, haveIonice;
    for (const auto &r : m_rules) {
        if (!r.enabled || r.pattern.isEmpty()) continue;
        // Same pattern AND same match type = the same set of processes.
        const QString key = r.pattern.toLower() + QChar(u'\u0000') + r.matchType;
        if (r.affinity) {
            if (haveAffinity.contains(key)) out[r.ruleId] << QStringLiteral("affinity");
            else                            haveAffinity.insert(key);
        }
        if (r.nice) {
            if (haveNice.contains(key)) out[r.ruleId] << QStringLiteral("nice");
            else                        haveNice.insert(key);
        }
        if (r.ioniceClass) {
            if (haveIonice.contains(key)) out[r.ruleId] << QStringLiteral("I/O priority");
            else                          haveIonice.insert(key);
        }
    }
    return out;
}

bool RuleEngine::isPbExempt(const QString &procName) const
{
    for (const auto &rule : m_rules)
        if (rule.pbExempt.value_or(false) && rule.matches(procName)) return true;
    return false;
}
