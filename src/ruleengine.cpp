#include "ruleengine.h"
#include "cpupark.h"
#include <QSet>
#include <cstring>
#include "utils.h"
#include "cputopology.h"
#include "verbose.h"
#include <QJsonArray>
#include <QRegularExpression>

// ── Rule ──────────────────────────────────────────────────────────────────────

bool Rule::matches(const QString &procName, const QString &cmdline) const
{
    if (!enabled || pattern.isEmpty()) return false;
    // Rules written before matchTarget existed have it defaulted to "name",
    // so their behaviour is unchanged.
    const QString subject = (matchTarget == QLatin1String("cmdline")) ? cmdline : procName;
    if (subject.isEmpty()) return false;
    if (matchType == QLatin1String("exact"))
        return subject == pattern;
    if (matchType == QLatin1String("regex")) {
        const QRegularExpression re(pattern);
        return re.isValid() && re.match(subject).hasMatch();
    }
    // "contains" (default)
    return subject.toLower().contains(pattern.toLower());
}

QJsonObject Rule::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("rule_id")]     = ruleId;
    obj[QStringLiteral("name")]        = name;
    obj[QStringLiteral("pattern")]     = pattern;
    obj[QStringLiteral("match_type")]  = matchType;
    obj[QStringLiteral("match_target")] = matchTarget;
    obj[QStringLiteral("affinity")]    = affinity ? QJsonValue(*affinity) : QJsonValue::Null;
    obj[QStringLiteral("nice")]        = nice     ? QJsonValue(*nice)     : QJsonValue::Null;
    obj[QStringLiteral("ionice_class")]= ioniceClass ? QJsonValue(*ioniceClass) : QJsonValue::Null;
    obj[QStringLiteral("ionice_level")]= ioniceLevel ? QJsonValue(*ioniceLevel) : QJsonValue::Null;
    obj[QStringLiteral("pb_exempt")]   = pbExempt   ? QJsonValue(*pbExempt)    : QJsonValue::Null;
    obj[QStringLiteral("allow_helper")]= allowHelper? QJsonValue(*allowHelper) : QJsonValue::Null;
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
    r.matchTarget = obj[QStringLiteral("match_target")].toString(QStringLiteral("name"));
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
    const auto ah = obj[QStringLiteral("allow_helper")];
    if (!ah.isNull() && ah.isBool()) r.allowHelper = ah.toBool();
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

// One line per rule+process for a failure that will otherwise repeat on every
// enforcement pass, twice a second, forever.
void RuleEngine::warnOnce(const Rule &rule, int pid, const QString &procName,
                          const QString &what, int err, const QString &attr)
{
    const QString key = rule.ruleId + QLatin1Char('|') + QString::number(pid)
                      + QLatin1Char('|') + what;
    if (m_permWarned.contains(key)) return;
    if (m_permWarned.size() > 2048) m_permWarned.clear();
    m_permWarned.insert(key);
    const QString why = err == EPERM
        ? Utils::describeAffinityError(EPERM, pid)
        : QString::fromLocal8Bit(strerror(err ? err : EINVAL));
    log(QStringLiteral("[Rule:%1] %2 NOT applied to %3(%4) — %5")
            .arg(rule.name, what, procName).arg(pid).arg(why));
    noteFailure(rule.ruleId, attr,
                QStringLiteral("%1 could not be applied to %2(%3): %4")
                    .arg(what, procName).arg(pid).arg(why));
}

QHash<QString, QHash<QString, QString>> RuleEngine::ruleFailures() const
{
    QMutexLocker lk(&m_failMux);
    return m_ruleFailures;
}

quint64 RuleEngine::failureGeneration() const
{
    QMutexLocker lk(&m_failMux);
    return m_failGen;
}

void RuleEngine::noteFailure(const QString &ruleId, const QString &attr, const QString &reason)
{
    QMutexLocker lk(&m_failMux);
    if (m_ruleFailures[ruleId].value(attr) == reason) return;   // unchanged, no repaint
    m_ruleFailures[ruleId][attr] = reason;
    ++m_failGen;
}

void RuleEngine::clearFailure(const QString &ruleId, const QString &attr)
{
    QMutexLocker lk(&m_failMux);
    auto it = m_ruleFailures.find(ruleId);
    if (it == m_ruleFailures.end() || !it->contains(attr)) return;
    it->remove(attr);
    if (it->isEmpty()) m_ruleFailures.erase(it);
    ++m_failGen;
}

QStringList RuleEngine::applyToProcess(int pid, const QString &procName, const QString &cmdline)
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
        if (!rule.matches(procName, cmdline)) continue;
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
            int setErr = 0;
            if (!have.isEmpty() && have == want) {
                // Already correct. No syscall, no log line — and whatever was
                // wrong before evidently is not any more.
                clearFailure(rule.ruleId, QStringLiteral("affinity"));
            } else if (Utils::setAffinity(pid, *rule.affinity, &setErr)) {
                clearFailure(rule.ruleId, QStringLiteral("affinity"));
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
                const QSet<int> offline = getOfflineCpuSet();
                // A permission failure is permanent, actionable, and repeats on
                // every pass — report it exactly once per rule+process.
                if (setErr == EPERM) {
                    const QString key = rule.ruleId + QLatin1Char('|') + QString::number(pid);
                    const bool mayEscalate = rule.allowHelper.value_or(false)
                                          || m_sessionHelper.contains(rule.ruleId);
                    if (mayEscalate) {
                        if (CpuPark::setAffinityViaHelper(pid, *rule.affinity)) {
                            // Next pass sees the value already correct and stays
                            // silent, so this logs once per actual change.
                            const QString msg =
                                QStringLiteral("[Rule:%1] affinity=%2 → %3(%4) (via privileged helper)")
                                    .arg(rule.name, *rule.affinity, procName).arg(pid);
                            log(msg); actions << msg;
                            m_permWarned.remove(key);
                        } else if (!m_permWarned.contains(key)) {
                            if (m_permWarned.size() > 2048) m_permWarned.clear();
                            m_permWarned.insert(key);
                            log(QStringLiteral("[Rule:%1] affinity=%2 NOT applied to %3(%4) — "
                                               "the privileged helper failed or is not installed "
                                               "(Gaming Mode tab → install helper).")
                                    .arg(rule.name, *rule.affinity, procName).arg(pid));
                        }
                    } else if (m_escalationCb && !m_escalationAsked.contains(rule.ruleId)) {
                        // Ask once per rule per session, never once per pid: a
                        // rule matching a browser would otherwise open 150 dialogs.
                        m_escalationAsked.insert(rule.ruleId);
                        m_escalationCb(rule.ruleId, rule.name, pid, procName, *rule.affinity);
                    } else if (!m_permWarned.contains(key)) {
                        if (m_permWarned.size() > 2048) m_permWarned.clear();
                        m_permWarned.insert(key);
                        log(QStringLiteral("[Rule:%1] affinity=%2 NOT applied to %3(%4) — %5")
                                .arg(rule.name, *rule.affinity, procName)
                                .arg(pid)
                                .arg(Utils::describeAffinityError(setErr, pid)));
                        noteFailure(rule.ruleId, QStringLiteral("affinity"),
                                    Utils::describeAffinityError(setErr, pid));
                    }
                }
                // Only the parked case is worth telling the user about: it is
                // actionable and cannot interleave with success. Anything else
                // is almost always ESRCH (the process exited between the
                // snapshot and the syscall) — noise, not a problem.
                else if (want.isEmpty() || !(want - offline).isEmpty()) {
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
                    noteFailure(rule.ruleId, QStringLiteral("affinity"),
                                QStringLiteral("every one of those CPUs is parked"));
                }
            }
        }
        if (rule.nice && !niceDone) {
            niceDone = true;
            int curNice = 0;
            const bool niceKnown = Utils::getNice(pid, curNice);
            if (niceKnown && curNice == *rule.nice) {
                // Already correct — same reasoning as affinity above.
                clearFailure(rule.ruleId, QStringLiteral("nice"));
            } else if (Utils::setNice(pid, *rule.nice)) {
                clearFailure(rule.ruleId, QStringLiteral("nice"));
                const QString msg = QStringLiteral("[Rule:%1] nice=%2 → %3(%4)")
                    .arg(rule.name).arg(*rule.nice).arg(procName).arg(pid);
                log(msg); actions << msg;
            } else {
                const int err = errno;
                const QString what = QStringLiteral("priority %1").arg(*rule.nice);
                const bool mayEscalate = rule.allowHelper.value_or(false)
                                      || m_sessionHelper.contains(rule.ruleId);
                // The helper has had renice-pid since long before set-affinity,
                // but only affinity ever used it — so a rule could set affinity
                // on a root-owned process and then fail to set its priority.
                if (err == EPERM && mayEscalate
                 && CpuPark::setProcessNiceViaHelper(pid, *rule.nice)) {
                    const QString msg = QStringLiteral("[Rule:%1] nice=%2 → %3(%4) (via privileged helper)")
                        .arg(rule.name).arg(*rule.nice).arg(procName).arg(pid);
                    log(msg); actions << msg;
                } else if (err == EPERM && !mayEscalate && m_escalationCb
                        && !m_escalationAsked.contains(rule.ruleId)) {
                    m_escalationAsked.insert(rule.ruleId);
                    m_escalationCb(rule.ruleId, rule.name, pid, procName, what);
                } else {
                    // Previously silent. A rule that cannot apply its priority —
                    // a process owned by someone else, or a value beyond
                    // RLIMIT_NICE — looked exactly like a rule doing nothing.
                    warnOnce(rule, pid, procName, what, err, QStringLiteral("nice"));
                }
            }
        }
        if (rule.ioniceClass && !ioniceDone) {
            ioniceDone = true;
            const int level = rule.ioniceLevel.value_or(0);
            int curClass = 0, curLevel = 0;
            const bool ioKnown = Utils::getIoNice(pid, curClass, curLevel);
            if (ioKnown && curClass == *rule.ioniceClass && curLevel == level) {
                // Already correct — same reasoning as affinity above.
                clearFailure(rule.ruleId, QStringLiteral("ionice"));
            } else if (Utils::setIoNice(pid, *rule.ioniceClass, level)) {
                clearFailure(rule.ruleId, QStringLiteral("ionice"));
                const QString msg = QStringLiteral("[Rule:%1] ionice class=%2 level=%3 → %4(%5)")
                    .arg(rule.name).arg(*rule.ioniceClass).arg(level).arg(procName).arg(pid);
                log(msg); actions << msg;
            } else {
                warnOnce(rule, pid, procName,
                         QStringLiteral("I/O priority class %1 level %2")
                             .arg(*rule.ioniceClass).arg(level), errno,
                         QStringLiteral("ionice"));
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
        const QString key = r.pattern.toLower() + QChar(u'\u0000') + r.matchType
                          + QChar(u'\u0000') + r.matchTarget;
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

bool RuleEngine::isPbExempt(const QString &procName, const QString &cmdline) const
{
    for (const auto &rule : m_rules)
        if (rule.pbExempt.value_or(false) && rule.matches(procName, cmdline)) return true;
    return false;
}
