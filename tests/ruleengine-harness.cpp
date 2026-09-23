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

    // ── 4. Another user's process: EPERM must be asked about once, not spammed ──
    //     pid 1 is root-owned, so an unprivileged run gets EPERM from the kernel
    //     without needing any privilege to set the test up.
    {
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("p", "root-rule", "2-5") });
        int asks = 0, logs = 0;
        re.setLogCallback([&](const QString &){ ++logs; });
        re.setEscalationCallback([&](QString, QString, int, QString, QString){ ++asks; });

        for (int p = 0; p < 10; ++p) re.applyToProcess(1, "plqprobe");
        check(asks == 1, "a rule needing root asks exactly ONCE, not once per pass");

        // A second process matched by the same rule must not ask again — a rule
        // matching a browser would otherwise open a dialog per process.
        for (int p = 0; p < 10; ++p) re.applyToProcess(2, "plqprobe");
        check(asks == 1, "…and not again for another process of the same rule");

        const int logsAfter = logs;
        for (int p = 0; p < 20; ++p) re.applyToProcess(1, "plqprobe");
        check(logs == logsAfter, "a permanently-failing rule does not flood the log");
        check(logs <= 2, "at most one explanation per process, not per pass");
    }

    // ── 5. With no escalation handler at all, still no flood ───────────────
    {
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("q", "root-rule-2", "2-5") });
        int logs = 0;
        re.setLogCallback([&](const QString &){ ++logs; });
        for (int p = 0; p < 20; ++p) re.applyToProcess(1, "plqprobe");
        check(logs == 1, "without a handler, EPERM is explained exactly once");
    }

    // ── 6. Telling apart processes that share a name ───────────────────────
    //     The real case: several python3.13 interpreters, only one of which is
    //     ComfyUI. A name rule cannot distinguish them; a cmdline rule can.
    {
        QJsonObject byName = mkRule("n", "all-python", "2-5");
        byName["pattern"] = "python3.13";
        QJsonObject byCmd = mkRule("c", "comfyui-only", "2-5");
        byCmd["pattern"] = "ComfyUI/main.py";
        byCmd["match_target"] = "cmdline";

        RuleEngine re; re.loadRules(QJsonArray{ byName, byCmd });
        const Rule nameRule = re.rules().at(0);
        const Rule cmdRule  = re.rules().at(1);

        const QString comfy = QStringLiteral("python3.13 ./ComfyUI/main.py --listen --port 8188");
        const QString other = QStringLiteral("python3.13 /usr/bin/some-other-tool.py");

        check(nameRule.matches("python3.13", comfy),  "name rule matches ComfyUI's python");
        check(nameRule.matches("python3.13", other),  "name rule ALSO matches the other python (the problem)");
        check(cmdRule.matches("python3.13", comfy),   "cmdline rule matches ComfyUI's python");
        check(!cmdRule.matches("python3.13", other),  "cmdline rule does NOT match the other python");
        check(!cmdRule.matches("python3.13", QString()),
              "a cmdline rule cannot match when the command line is unknown");

        // match_target must survive a JSON round trip, or it silently reverts
        // to matching the name and starts hitting every python again.
        const Rule back = Rule::fromJson(cmdRule.toJson());
        check(back.matchTarget == QLatin1String("cmdline"), "match_target round-trips through JSON");
        check(back.matches("python3.13", comfy) && !back.matches("python3.13", other),
              "…and still discriminates after the round trip");

        // Old rules with no match_target must keep matching by name.
        QJsonObject legacy = mkRule("l", "legacy", "2-5");
        legacy["pattern"] = "python3.13";
        legacy.remove("match_target");
        check(Rule::fromJson(legacy).matches("python3.13", other),
              "a rule saved before this feature still matches by name");
    }

    // ── 7. A rule that cannot apply is reported as failing, and recovers ───
    //     pid 1 is root-owned, so nice on it fails with EPERM unprivileged.
    {
        QJsonObject r = mkRule("f", "root-nice", nullptr);
        r["nice"] = 7;
        RuleEngine re; re.loadRules(QJsonArray{ r });
        check(re.ruleFailures().isEmpty(), "no failures before anything is applied");

        re.applyToProcess(1, "plqprobe");
        const auto f = re.ruleFailures();
        check(f.value("f").contains(QStringLiteral("nice")),
              "a rule that cannot apply its priority is marked failing");
        check(f.value("f").value(QStringLiteral("nice")).contains(QStringLiteral("root")),
              "…and the reason names the owner");

        const quint64 gen = re.failureGeneration();
        re.applyToProcess(1, "plqprobe");
        check(re.failureGeneration() == gen,
              "repeating the same failure does not churn the generation (no repaint storm)");

        // Applying successfully to a process we DO own clears the mark.
        re.applyToProcess(pid, "plqprobe");
        check(!re.ruleFailures().value("f").contains(QStringLiteral("nice")),
              "the mark clears once the rule applies successfully");
        check(re.failureGeneration() > gen, "…and that bumps the generation so the table repaints");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
