// scan-interval-harness.cpp — the configurable scan interval (monitor.scan_interval_ms).
//
// Checks that the value is clamped to 500 ms–30 s, that a 30 s interval does
// NOT make stop() slow (the sleep must be interruptible, or quitting the app
// would hang and ~MainWindow's wait(3000) would time out), and that a GUI
// request wakes the scanner instead of waiting out the interval.
//
//   /usr/lib/qt6/moc -I../src ../src/processmonitor.h -o moc_processmonitor.cpp
//   g++ -std=c++17 -fPIC -pie -I../src $(pkg-config --cflags Qt6Core) \
//       scan-interval-harness.cpp moc_processmonitor.cpp ../src/processmonitor.cpp \
//       ../src/ruleengine.cpp ../src/probalance.cpp ../src/utils.cpp \
//       ../src/cpupark.cpp ../src/sensors.cpp ../src/cputopology.cpp \
//       $(pkg-config --libs Qt6Core) -o scan-interval-harness
//   cp /usr/bin/sleep ./plqprobe && ./scan-interval-harness ./plqprobe
//
// Only the probe matches the rule; ProBalance off; no default affinity.
#include "processmonitor.h"
#include "ruleengine.h"
#include "probalance.h"
#include "utils.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

bool gVerbose = false;
static int failures = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static QJsonObject cfgWith(int scanMs) {
    QJsonObject cfg;
    cfg["cpu"]     = QJsonObject{{"default_affinity", QJsonValue::Null}};
    cfg["monitor"] = QJsonObject{{"rule_enforce_interval_ms", 500},
                                 {"display_refresh_interval_ms", 2000},
                                 {"scan_interval_ms", scanMs}};
    cfg["probalance"]        = QJsonObject{{"enabled", false}};
    cfg["show_temperatures"] = false;
    return cfg;
}

static pid_t spawn(const char *bin) {
    const pid_t p = fork();
    if (p == 0) { execl(bin, bin, "120", (char*)nullptr); _exit(127); }
    return p;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <probe-binary>\n", argv[0]); return 2; }

    // 1. clamping
    check(ProcessMonitor::scanIntervalMs(QJsonObject{}) == 500, "missing key defaults to 500 ms");
    check(ProcessMonitor::scanIntervalMs(cfgWith(100))    == 500,   "100 ms clamps up to 500");
    check(ProcessMonitor::scanIntervalMs(cfgWith(999999)) == 30000, "huge value clamps to 30 s");
    check(ProcessMonitor::scanIntervalMs(cfgWith(5000))   == 5000,  "in-range value is kept");

    QJsonObject rule{{"rule_id","s"},{"name","s"},{"pattern","plqprobe"},
                     {"match_type","contains"},{"affinity","2-5"},{"enabled",true}};
    RuleEngine re; re.loadRules(QJsonArray{ rule });
    ProBalance pb(cfgWith(30000)["probalance"].toObject());
    ProcessMonitor mon(&re, &pb, cfgWith(30000));
    mon.start();
    QThread::msleep(1500);                 // first scan done; now in a 30 s sleep

    // 2. a process started mid-sleep is NOT seen until the next scan …
    const pid_t probe = spawn(argv[1]);
    QThread::msleep(1500);
    check(Utils::getAffinityStr(probe) != QStringLiteral("2-5"),
          "30 s interval: a new process waits for the next scan");

    // 3. … but a GUI request wakes the scanner at once
    QElapsedTimer t; t.start();
    mon.reapplyRulesNow();
    while (t.elapsed() < 3000 && Utils::getAffinityStr(probe) != QStringLiteral("2-5"))
        QThread::msleep(20);
    printf("      rule applied %lld ms after the request\n", (long long)t.elapsed());
    check(Utils::getAffinityStr(probe) == QStringLiteral("2-5"),
          "reapplyRulesNow() wakes the scanner (no 30 s wait)");

    // 4. stop() is not held up by the long interval
    QThread::msleep(500);                  // back in the 30 s sleep
    t.restart();
    mon.stop();
    const bool joined = mon.wait(3000);
    printf("      stop() + wait() took %lld ms\n", (long long)t.elapsed());
    check(joined && t.elapsed() < 1000, "stop() returns promptly with a 30 s interval");

    kill(probe, SIGKILL); int st = 0; waitpid(probe, &st, 0);
    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
