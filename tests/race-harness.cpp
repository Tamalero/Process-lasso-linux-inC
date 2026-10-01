// race-harness.cpp — the GUI-thread/monitor-thread races fixed after 1.5.0,
// checked under ThreadSanitizer. Edits rules on one thread (the Rules tab's
// job) while another thread enforces them (the monitor's job), then drives a
// live ProcessMonitor with the GUI-side requests that used to run inline.
//
// Build with TSan, then run against a probe binary (name ≤ 15 chars, see
// monitor-harness.cpp):
//   /usr/lib/qt6/moc -I../src ../src/processmonitor.h -o moc_processmonitor.cpp
//   g++ -std=c++17 -fPIC -pie -g -O1 -fsanitize=thread -I../src \
//       $(pkg-config --cflags Qt6Core) race-harness.cpp moc_processmonitor.cpp \
//       ../src/processmonitor.cpp ../src/ruleengine.cpp ../src/probalance.cpp \
//       ../src/utils.cpp ../src/cpupark.cpp ../src/sensors.cpp ../src/cputopology.cpp \
//       $(pkg-config --libs Qt6Core) -o race-harness
//   cp /usr/bin/sleep ./plqprobe && ./race-harness ./plqprobe
// Any "WARNING: ThreadSanitizer: data race" in the output is a failure.
//
// Compile with -DENGINE_ONLY (and only ruleengine.cpp utils.cpp cpupark.cpp
// cputopology.cpp) to run part 1 alone — that also builds against the 1.5.0
// engine, which is how the race was shown to be real before the fix.
//
// ⚠️ Never calls resetAllAffinities(): that rewrites the affinity of EVERY
// process the monitor has seen, i.e. the whole live desktop. ProBalance is
// disabled and default_affinity is null for the same reason — only the probe
// is ever acted on.
#include "ruleengine.h"
#include "utils.h"
#ifndef ENGINE_ONLY
#include "processmonitor.h"
#include "probalance.h"
#endif
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

bool gVerbose = false;

static int failures = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static QJsonObject mkRule(const char *id, const char *pattern, const char *aff) {
    QJsonObject r;
    r["rule_id"] = id; r["name"] = id; r["pattern"] = pattern;
    r["match_type"] = "contains"; r["affinity"] = aff; r["enabled"] = true;
    return r;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <probe-binary>\n", argv[0]); return 2; }
    const pid_t probe = fork();
    if (probe == 0) { execl(argv[1], argv[1], "120", (char*)nullptr); _exit(127); }
    QThread::msleep(100);
    const QString name = QStringLiteral("plqprobe");

    // ── 1. Rules edited on one thread while another enforces them ──────────
    {
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("a", "plqprobe", "2-5") });
        std::atomic<bool> stop{false};
        std::atomic<long> passes{0};
        std::thread monitor([&]{
            while (!stop) {
                re.applyToProcess(probe, name);
                re.isPbExempt(name);
                ++passes;
            }
        });
        const Rule base = re.rules().at(0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (int i = 0; std::chrono::steady_clock::now() < until; ++i) {
            Rule r = base; r.affinity = (i & 1) ? QStringLiteral("6-9") : QStringLiteral("2-5");
            re.updateRule(r);                                   // Rules tab: edit
            Rule extra; extra.ruleId = QStringLiteral("x"); extra.pattern = QStringLiteral("zz-never");
            re.addRule(extra);                                  // add …
            re.removeRule(QStringLiteral("x"));                 // … delete
            re.allowHelperForSession(QStringLiteral("a"));      // escalation prompt
        }
        stop = true; monitor.join();
        printf("      engine: %ld enforcement passes during concurrent edits\n", passes.load());
        check(passes > 100, "enforcement kept running while rules were edited");
    }

#ifndef ENGINE_ONLY
    // ── 2. matchesAny(): a matching rule that is already correct ───────────
    {
        RuleEngine re;
        re.loadRules(QJsonArray{ mkRule("m", "plqprobe", "2-5") });
        re.applyToProcess(probe, name);                         // make it correct
        const bool silent = re.applyToProcess(probe, name).isEmpty();
        check(silent, "already-correct rule returns no actions (1.4.1 behaviour)");
        check(re.matchesAny(name), "…but matchesAny() still reports the match");
        check(!re.matchesAny(QStringLiteral("zz-nothing")), "matchesAny() is false with no match");
    }

    // ── 3. GUI-side requests hammered at a running monitor ─────────────────
    {
        QJsonObject cfg;
        cfg["cpu"]     = QJsonObject{{"default_affinity", QJsonValue::Null}};
        cfg["monitor"] = QJsonObject{{"rule_enforce_interval_ms", 100},
                                     {"display_refresh_interval_ms", 100000}};
        cfg["probalance"]        = QJsonObject{{"enabled", false}};
        cfg["show_temperatures"] = false;
        RuleEngine re; re.loadRules(QJsonArray{ mkRule("g", "plqprobe", "2-5") });
        ProBalance pb(cfg["probalance"].toObject());
        ProcessMonitor mon(&re, &pb, cfg);
        mon.start();
        const Rule base = re.rules().at(0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        long reqs = 0;
        while (std::chrono::steady_clock::now() < until) {
            mon.reapplyAllDefaults();          // every rule edit calls this
            mon.setGamingMode(reqs & 1, true); // toggled from the Gaming tab
            (void)mon.throttledPids();         // read by onSnapshot()
            re.updateRule(base);               // Rules tab edit
            ++reqs;
            QThread::usleep(200);
        }
        mon.setGamingMode(false, false);
        QThread::msleep(300);                  // let the last requests drain
        check(Utils::getAffinityStr(probe) == QStringLiteral("2-5"),
              "monitor still enforces the rule after the request storm");
        mon.stop(); mon.wait(3000);
        printf("      monitor: %ld rounds of GUI requests\n", reqs);
    }
#endif

    kill(probe, SIGKILL); int st = 0; waitpid(probe, &st, 0);
    printf("\n%s (%d failure%s) — also check above for ThreadSanitizer reports\n",
           failures ? "FAILURES" : "ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
