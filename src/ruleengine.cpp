#include "ruleengine.h"
#include "cpupark.h"
#include <QSet>
#include <cstring>
#include "utils.h"
#include "cputopology.h"
#include "verbose.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <utility>

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

// Called before the monitor starts (MainWindow ctor) and by the harnesses, so
// touching the monitor-owned m_affinityWarned here is safe.
void RuleEngine::loadRules(const QJsonArray &arr)
{
    QList<Rule> loaded;
    for (const auto &v : arr)
        if (v.isObject()) loaded.append(Rule::fromJson(v.toObject()));
    m_affinityWarned.clear();
    QMutexLocker lk(&m_rulesMux);
    m_rules = loaded;
}

QJsonArray RuleEngine::toJsonArray() const
{
    QList<Rule> rules; snapshot(rules);
    QJsonArray arr;
    for (const auto &r : rules) arr.append(r.toJson());
    return arr;
}

void RuleEngine::snapshot(QList<Rule> &rules, QSet<QString> *sessionHelper) const
{
    QMutexLocker lk(&m_rulesMux);
    rules = m_rules;
    if (sessionHelper) *sessionHelper = m_sessionHelper;
}

// Mutators never write into the live buffer: they edit a copy and swap it in.
// The monitor may still be reading the old buffer through its snapshot, and
// QList decides "shared or not" with a relaxed load — editing in place whenever
// it happened to read refcount 1 was flagged by ThreadSanitizer. The copy here
// is always detached (m_rules still holds the old buffer), so no buffer is ever
// written after another thread could have seen it.
void RuleEngine::allowHelperForSession(const QString &ruleId)
{
    QMutexLocker lk(&m_rulesMux);
    QSet<QString> next = m_sessionHelper;
    next.insert(ruleId);
    m_sessionHelper = next;
}

void RuleEngine::addRule(const Rule &rule)
{
    QMutexLocker lk(&m_rulesMux);
    QList<Rule> next = m_rules;
    next.append(rule);
    m_rules = next;
}

void RuleEngine::removeRule(const QString &ruleId)
{
    QMutexLocker lk(&m_rulesMux);
    QList<Rule> next = m_rules;
    next.removeIf([&](const Rule &r){ return r.ruleId == ruleId; });
    m_rules = next;
}

void RuleEngine::updateRule(const Rule &rule)
{
    QMutexLocker lk(&m_rulesMux);
    QList<Rule> next = m_rules;
    for (auto &r : next) {
        if (r.ruleId == rule.ruleId) { r = rule; break; }
    }
    m_rules = next;
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
    // Iterate a copy: the GUI thread may edit the rules mid-pass (see header).
    QList<Rule> rules; QSet<QString> sessionHelper;
    snapshot(rules, &sessionHelper);
    for (const auto &rule : std::as_const(rules)) {
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
                // Still ours to give back if the rule goes: this is how every
                // child of a pinned browser is born.
                ownAffinity(pid, {}, want);
            } else if (Utils::setAffinity(pid, *rule.affinity, &setErr)) {
                clearFailure(rule.ruleId, QStringLiteral("affinity"));
                // A mask read while CPUs are parked is truncated — never keep
                // it as the one to restore (see captureOriginal()).
                ownAffinity(pid, getOfflineCpuSet().isEmpty() ? have : QSet<int>{}, want);
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
                                          || sessionHelper.contains(rule.ruleId);
                    if (mayEscalate) {
                        if (CpuPark::setAffinityViaHelper(pid, *rule.affinity)) {
                            ownAffinity(pid, getOfflineCpuSet().isEmpty() ? have : QSet<int>{}, want);
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
            const std::optional<int> niceBefore =
                niceKnown ? std::optional<int>(curNice) : std::nullopt;
            if (niceKnown && curNice == *rule.nice) {
                // Already correct — same reasoning as affinity above.
                clearFailure(rule.ruleId, QStringLiteral("nice"));
                ownNice(pid, std::nullopt, *rule.nice);
            } else if (Utils::setNice(pid, *rule.nice)) {
                clearFailure(rule.ruleId, QStringLiteral("nice"));
                ownNice(pid, niceBefore, *rule.nice);
                const QString msg = QStringLiteral("[Rule:%1] nice=%2 → %3(%4)")
                    .arg(rule.name).arg(*rule.nice).arg(procName).arg(pid);
                log(msg); actions << msg;
            } else {
                const int err = errno;
                const QString what = QStringLiteral("priority %1").arg(*rule.nice);
                const bool mayEscalate = rule.allowHelper.value_or(false)
                                      || sessionHelper.contains(rule.ruleId);
                // The helper has had renice-pid since long before set-affinity,
                // but only affinity ever used it — so a rule could set affinity
                // on a root-owned process and then fail to set its priority.
                if (err == EPERM && mayEscalate
                 && CpuPark::setProcessNiceViaHelper(pid, *rule.nice)) {
                    ownNice(pid, niceBefore, *rule.nice);
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
                ownIoNice(pid, std::nullopt, *rule.ioniceClass, level);
            } else if (Utils::setIoNice(pid, *rule.ioniceClass, level)) {
                clearFailure(rule.ruleId, QStringLiteral("ionice"));
                ownIoNice(pid, ioKnown ? std::optional<std::pair<int, int>>({curClass, curLevel})
                                       : std::nullopt,
                          *rule.ioniceClass, level);
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

// First write wins for `orig`: later passes only move `set`, so an edited rule
// still restores what the process had before ANY rule touched it.
void RuleEngine::ownAffinity(int pid, const QSet<int> &before, const QSet<int> &set)
{
    auto it = m_ownedAffinity.find(pid);
    if (it == m_ownedAffinity.end()) m_ownedAffinity.insert(pid, { before, set });
    else                             it->set = set;
}

void RuleEngine::ownNice(int pid, std::optional<int> before, int set)
{
    auto it = m_ownedNice.find(pid);
    if (it == m_ownedNice.end()) m_ownedNice.insert(pid, { before, set });
    else                         it->set = set;
}

void RuleEngine::ownIoNice(int pid, std::optional<std::pair<int, int>> before, int cls, int lvl)
{
    auto it = m_ownedIoNice.find(pid);
    if (it == m_ownedIoNice.end()) m_ownedIoNice.insert(pid, { before, cls, lvl });
    else                           { it->cls = cls; it->lvl = lvl; }
}

int RuleEngine::releaseUnclaimed(int pid, const QString &procName, const QString &cmdline,
                                 const QString &defaultAffinity)
{
    const bool ownsAff  = m_ownedAffinity.contains(pid);
    const bool ownsNice = m_ownedNice.contains(pid);
    const bool ownsIo   = m_ownedIoNice.contains(pid);
    if (!ownsAff && !ownsNice && !ownsIo) return 0;

    // Claimed by ANY enabled matching rule, not just the first: a surviving rule
    // keeps the attribute and applyToProcess() moves it to the new value.
    bool wantsAff = false, wantsNice = false, wantsIo = false;
    QList<Rule> rules; snapshot(rules);
    for (const auto &r : std::as_const(rules)) {
        if (!r.matches(procName, cmdline)) continue;
        wantsAff  |= r.affinity.has_value();
        wantsNice |= r.nice.has_value();
        wantsIo   |= r.ioniceClass.has_value();
    }

    // In every branch the record is dropped whether or not the write works:
    // retrying a failing restore on every rule change would be its own flood.
    // And if the current value is no longer what the rule set, someone else —
    // the user, the program itself — has changed it since; that is theirs.
    int restored = 0;
    if (ownsAff && !wantsAff) {
        const OwnedAffinity o = m_ownedAffinity.take(pid);
        const QSet<int> have = Utils::cpulistToSet(Utils::getAffinityStr(pid));
        if (have == o.set) {
            QSet<int> target = defaultAffinity.isEmpty()
                ? o.orig : Utils::cpulistToSet(defaultAffinity);
            if (target.isEmpty())
                for (int i = 0; i < Utils::getCpuCount(); ++i) target.insert(i);
            const QString list = Utils::cpusetToCpulist(target);
            if (target != have && Utils::setAffinity(pid, list)) {
                ++restored;
                VLOG("release: affinity %s -> %s(%d)", qPrintable(list),
                     qPrintable(procName), pid);
            }
        }
    }
    if (ownsNice && !wantsNice) {
        const OwnedNice o = m_ownedNice.take(pid);
        int cur = 0;
        // Unknown original: 0 is what every process starts at.
        const int target = o.orig.value_or(0);
        if (Utils::getNice(pid, cur) && cur == o.set && cur != target
         && Utils::setNice(pid, target)) {
            ++restored;
            VLOG("release: nice %d -> %s(%d)", target, qPrintable(procName), pid);
        }
    }
    if (ownsIo && !wantsIo) {
        const OwnedIoNice o = m_ownedIoNice.take(pid);
        int cls = 0, lvl = 0;
        // Unknown original: class 0 ("none") — I/O priority follows nice again.
        const auto target = o.orig.value_or(std::pair<int, int>{ 0, 0 });
        if (Utils::getIoNice(pid, cls, lvl) && cls == o.cls && lvl == o.lvl
         && std::pair<int, int>{ cls, lvl } != target
         && Utils::setIoNice(pid, target.first, target.second)) {
            ++restored;
            VLOG("release: ionice %d/%d -> %s(%d)", target.first, target.second,
                 qPrintable(procName), pid);
        }
    }
    return restored;
}

void RuleEngine::forgetDeadPids(const QSet<int> &alive)
{
    // A recycled pid must not inherit a dead process's "original" values.
    const auto prune = [&](auto &hash) {
        for (auto it = hash.begin(); it != hash.end(); )
            it = alive.contains(it.key()) ? std::next(it) : hash.erase(it);
    };
    prune(m_ownedAffinity);
    prune(m_ownedNice);
    prune(m_ownedIoNice);
}

QHash<QString, QStringList> RuleEngine::shadowedAttributes() const
{
    QHash<QString, QStringList> out;
    QSet<QString> haveAffinity, haveNice, haveIonice;
    QList<Rule> rules; snapshot(rules);
    for (const auto &r : std::as_const(rules)) {
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
    QList<Rule> rules; snapshot(rules);
    for (const auto &rule : std::as_const(rules))
        if (rule.pbExempt.value_or(false) && rule.matches(procName, cmdline)) return true;
    return false;
}

bool RuleEngine::matchesAny(const QString &procName, const QString &cmdline) const
{
    QList<Rule> rules; snapshot(rules);
    for (const auto &rule : std::as_const(rules))
        if (rule.matches(procName, cmdline)) return true;
    return false;
}
