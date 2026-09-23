// ruleengine-harness.cpp — regression test for the v1.4.1 GUI-starvation bug.
//
// Rule enforcement runs for every matching pid twice a second. If it writes and
// logs even when the value is ALREADY correct, ~150 browser processes produce
// ~300 log lines/second on the GUI thread and the app stops responding. This
// asserts the steady state is completely silent, and that a rule which really
// does need applying still applies and still logs once.
//
// Build and run (pass the pid of a process to act on):
//   g++ -std=c++17 -fPIC -pie -I../src $(pkg-config --cflags Qt6Core) \
//       ruleengine-harness.cpp ../src/ruleengine.cpp ../src/utils.cpp \
//       ../src/cpupark.cpp ../src/cputopology.cpp \
//       $(pkg-config --libs Qt6Core) -o ruleengine-harness
//   sleep 60 & ./ruleengine-harness $!
//
// Acts ONLY on the pid you pass. Give it a throwaway process, never a real one.
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

static QJsonObject mkRule(const char *id, const char *name, const char *affinity)
{
    QJsonObject r;
    r["rule_id"] = id; r["name"] = name; r["pattern"] = "plqprobe";
    r["match_type"] = "contains"; r["enabled"] = true;
    if (affinity) r["affinity"] = affinity;
    return r;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <pid>\n", argv[0]); return 2; }
    const int pid = QString::fromLatin1(argv[1]).toInt();

    // ── 1. A rule that needs applying applies, and then goes quiet ──────────
    {
        QJsonObject r = mkRule("t1", "probe", "2-5");
        r["nice"] = 5; r["ionice_class"] = 2; r["ionice_level"] = 4;
        int lines = 0;
        RuleEngine re; re.loadRules(QJsonArray{ r });
        re.setLogCallback([&](const QString &){ ++lines; });

        re.applyToProcess(pid, "plqprobe");
        check(lines > 0, "a rule that needs applying does apply, and logs");
        check(Utils::getAffinityStr(pid) == QStringLiteral("2-5"), "the value actually took");

        for (int p = 0; p < 10; ++p) { lines = 0; re.applyToProcess(pid, "plqprobe"); }
        check(lines == 0, "steady state is SILENT once the value matches");
    }

    // ── 2. Two enabled rules setting the same attribute must not fight ─────
    //     Before v1.4.3 both applied on every pass, so the mask flipped back
    //     and forth twice a second and each flip logged — a flood the no-op
    //     skip cannot damp, because the value genuinely keeps changing.
    {
        Utils::setAffinity(pid, QStringLiteral("0-31"));
        int lines = 0;
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("a", "first",  "2-5"),
                                 mkRule("b", "second", "8-11") });
        re.setLogCallback([&](const QString &){ ++lines; });

        re.applyToProcess(pid, "plqprobe");
        const QString afterFirst = Utils::getAffinityStr(pid);
        check(afterFirst == QStringLiteral("2-5"), "the FIRST matching rule wins");

        bool stable = true;
        for (int p = 0; p < 10; ++p) {
            lines = 0;
            re.applyToProcess(pid, "plqprobe");
            if (Utils::getAffinityStr(pid) != afterFirst) stable = false;
        }
        check(stable, "the value does not oscillate between the two rules");
        check(lines == 0, "two conflicting rules produce NO log flood");

        const auto shadow = re.shadowedAttributes();
        check(shadow.value(QStringLiteral("b")).contains(QStringLiteral("affinity")),
              "the losing rule's affinity is reported as shadowed");
        check(!shadow.contains(QStringLiteral("a")), "the winning rule is not shadowed");
    }

    // ── 3. Different attributes on the same pattern still combine ──────────
    {
        QJsonObject onlyNice = mkRule("n", "nice-only", nullptr);
        onlyNice["nice"] = 7;
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("a", "aff-only", "2-5"), onlyNice });
        re.applyToProcess(pid, "plqprobe");
        int n = 0; Utils::getNice(pid, n);
        check(Utils::getAffinityStr(pid) == QStringLiteral("2-5") && n == 7,
              "affinity from one rule and nice from another both apply");
        check(re.shadowedAttributes().isEmpty(), "non-overlapping attributes are not shadowed");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
