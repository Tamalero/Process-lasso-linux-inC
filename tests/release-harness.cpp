// release-harness.cpp — a deleted rule must give back what it set (v1.5.3).
//
// Until 1.5.3 deleting, disabling or re-pointing a rule left every process it
// had touched exactly as the rule set it: affinity, nice and ionice stayed until
// the process exited. Deleting the firefox/chromium rules therefore looked like
// the delete had not happened. RuleEngine::releaseUnclaimed() undoes it.
//
// Build and run (pass the pids of two throwaway processes):
//   g++ -std=c++17 -fPIC -pie -I../src $(pkg-config --cflags Qt6Core) \
//       release-harness.cpp ../src/ruleengine.cpp ../src/utils.cpp \
//       ../src/cpupark.cpp ../src/cputopology.cpp \
//       $(pkg-config --libs Qt6Core) -o release-harness
//   sleep 120 & A=$!; taskset -c 2-5 sleep 120 & B=$!; ./release-harness $A $B
//
// Acts ONLY on the pids you pass. Give it throwaway processes, never real ones.
// B must already be on exactly 2-5: it stands in for a browser child that was
// born pinned because its parent was.
#include "ruleengine.h"
#include "utils.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>

bool gVerbose = false;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static QJsonObject mkRule(const char *id, const char *affinity)
{
    QJsonObject r;
    r["rule_id"] = id; r["name"] = id; r["pattern"] = "plqprobe";
    r["match_type"] = "contains"; r["enabled"] = true;
    if (affinity) r["affinity"] = affinity;
    return r;
}

static QString aff(int pid) { return Utils::getAffinityStr(pid); }
static int niceOf(int pid) { int n = 99; Utils::getNice(pid, n); return n; }

// Back to a known start: every CPU, nice 0, ionice none.
static void reset(int pid, const QString &all)
{
    Utils::setAffinity(pid, all);
    Utils::setNice(pid, 0);
    Utils::setIoNice(pid, 0, 0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: %s <pid> <pid-pinned-to-2-5>\n", argv[0]); return 2; }
    const int a = QString::fromLatin1(argv[1]).toInt();
    const int b = QString::fromLatin1(argv[2]).toInt();
    const QString all = QStringLiteral("0-%1").arg(Utils::getCpuCount() - 1);
    const QSet<int> allSet = Utils::cpulistToSet(all);
    if (Utils::cpulistToSet(aff(b)) != Utils::cpulistToSet("2-5")) {
        fprintf(stderr, "second pid must start on exactly 2-5 (taskset -c 2-5)\n"); return 2;
    }
    reset(a, all);

    // ── 1. Delete: affinity, nice and ionice all come back ──────────────────
    {
        QJsonObject r = mkRule("t1", "2-5");
        r["nice"] = 5; r["ionice_class"] = 2; r["ionice_level"] = 4;
        RuleEngine re; re.loadRules(QJsonArray{ r });
        re.applyToProcess(a, "plqprobe");
        check(aff(a) == "2-5" && niceOf(a) == 5, "rule applied");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "nothing released while the rule exists");
        re.removeRule("t1");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 3, "delete releases all three attributes");
        check(Utils::cpulistToSet(aff(a)) == allSet, "affinity restored to what it was before");
        check(niceOf(a) == 0, "nice restored");
        int c = -1, l = -1; Utils::getIoNice(a, c, l);
        check(c == 0, "ionice restored");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "second release is a no-op");
    }

    // ── 2. Born pinned: unknown original falls back to every CPU ────────────
    {
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t2", "2-5") });
        const auto acts = re.applyToProcess(b, "plqprobe");
        check(acts.isEmpty(), "already-correct process is not written");
        re.removeRule("t2");
        check(re.releaseUnclaimed(b, "plqprobe", {}, {}) == 1, "but is still released");
        check(Utils::cpulistToSet(aff(b)) == allSet, "inherited pin falls back to every CPU");
    }
    reset(a, all);

    // ── 3. Someone else changed it since: leave it alone ────────────────────
    {
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t3", "2-5") });
        re.applyToProcess(a, "plqprobe");
        Utils::setAffinity(a, "0-1");                // the user, by hand
        re.removeRule("t3");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "a manual change is not released");
        check(aff(a) == "0-1", "the manual value survives");
    }
    reset(a, all);

    // ── 4. Disabled, and edited-then-deleted ────────────────────────────────
    {
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t4", "2-5") });
        re.applyToProcess(a, "plqprobe");
        Rule edited = re.rules().first(); edited.affinity = QStringLiteral("6-7");
        re.updateRule(edited);
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "an edited rule still claims it");
        re.applyToProcess(a, "plqprobe");
        check(aff(a) == "6-7", "edit applied");
        Rule off = re.rules().first(); off.enabled = false;
        re.updateRule(off);
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 1, "disabling releases");
        check(Utils::cpulistToSet(aff(a)) == allSet,
              "restores the value from before ANY rule, not the pre-edit one");
    }
    reset(a, all);

    // ── 5. Another rule still sets affinity: not released ───────────────────
    {
        QJsonObject other = mkRule("t5b", "8-9");
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t5a", "2-5"), other });
        re.applyToProcess(a, "plqprobe");
        re.removeRule("t5a");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "a surviving rule keeps the claim");
        re.applyToProcess(a, "plqprobe");
        check(aff(a) == "8-9", "and moves it to its own value");
    }
    reset(a, all);

    // ── 6. A default affinity wins over the original ────────────────────────
    {
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t6", "2-5") });
        re.applyToProcess(a, "plqprobe");
        re.removeRule("t6");
        re.releaseUnclaimed(a, "plqprobe", {}, QStringLiteral("10-11"));
        check(aff(a) == "10-11", "default affinity applied on release");
    }
    reset(a, all);

    // ── 7. Dead pids are forgotten ──────────────────────────────────────────
    {
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("t7", "2-5") });
        re.applyToProcess(a, "plqprobe");
        re.forgetDeadPids({});                      // as if `a` had exited
        re.removeRule("t7");
        check(re.releaseUnclaimed(a, "plqprobe", {}, {}) == 0, "a forgotten pid is not touched");
    }
    reset(a, all);

    printf("\n%s — %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
