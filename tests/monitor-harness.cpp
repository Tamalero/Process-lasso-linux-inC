// monitor-harness.cpp — drives ProcessMonitor headless (no GUI, no config file)
// against a process the harness spawns itself. Covers rule-driven affinity and
// the 30-second manual override that keeps a hand-set affinity from being
// reverted by the next enforcement pass.
//
// ProcessMonitor is a QObject, so it needs a moc pass:
//   /usr/lib/qt6/moc -I../src ../src/processmonitor.h -o moc_processmonitor.cpp
//   g++ -std=c++17 -fPIC -pie -I../src $(pkg-config --cflags Qt6Core) \
//       monitor-harness.cpp moc_processmonitor.cpp ../src/processmonitor.cpp \
//       ../src/ruleengine.cpp ../src/probalance.cpp ../src/utils.cpp \
//       ../src/cpupark.cpp ../src/sensors.cpp ../src/cputopology.cpp \
//       $(pkg-config --libs Qt6Core) -o monitor-harness
//   cp /usr/bin/sleep ./plqprobe && ./monitor-harness ./plqprobe
//
// The probe binary's name must be <= 15 chars: the kernel truncates comm to 15,
// so a longer name can never match the rule pattern and the test silently
// proves nothing. Learned the hard way.
//
// Uses a rule pattern that matches only the probe, so nothing else is touched.
#include "processmonitor.h"
#include "ruleengine.h"
#include "probalance.h"
#include "utils.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

bool gVerbose = false;

static QString affOf(int pid) { return Utils::getAffinityStr(pid); }

int failures = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <probe-binary>\n", argv[0]); return 2; }

    const pid_t probe = fork();
    if (probe == 0) { execl(argv[1], argv[1], "60", (char*)nullptr); _exit(127); }
    printf("probe pid=%d start=%s\n", probe, qPrintable(affOf(probe)));

    QJsonObject rule;
    rule["rule_id"] = "t1"; rule["name"] = "probe";
    rule["pattern"] = "plqprobe"; rule["match_type"] = "contains";
    rule["affinity"] = "2-5";     rule["enabled"] = true;

    QJsonObject cfg;
    cfg["cpu"]     = QJsonObject{{"default_affinity", QJsonValue::Null}};
    cfg["monitor"] = QJsonObject{{"rule_enforce_interval_ms", 500},
                                 {"display_refresh_interval_ms", 100000}};
    cfg["probalance"]        = QJsonObject{{"enabled", false}};
    cfg["show_temperatures"] = false;

    RuleEngine re;  re.loadRules(QJsonArray{ rule });
    ProBalance  pb(cfg["probalance"].toObject());
    ProcessMonitor mon(&re, &pb, cfg);
    mon.start();

    // 1. the rule takes effect
    for (int i = 0; i < 30 && affOf(probe) != QStringLiteral("2-5"); ++i)
        QThread::msleep(100);
    check(affOf(probe) == QStringLiteral("2-5"), "rule applies affinity 2-5 to the probe");

    // 2. a manual change WITH an override must survive re-enforcement.
    //    This is the path that silently did nothing: the override was inserted
    //    from another thread with no lock, so the monitor could miss it and
    //    revert the change on the very next pass.
    Utils::setAffinity(probe, QStringLiteral("0-31"));
    mon.setManualAffinityOverride(probe, 4.0);
    bool held = true;
    for (int i = 0; i < 25; ++i) {           // 2.5 s, i.e. ~5 enforcement passes
        QThread::msleep(100);
        if (affOf(probe) != QStringLiteral("0-31")) { held = false; break; }
    }
    check(held, "manual change survives rule re-enforcement while the override is live");

    // 3. once the override expires the rule takes over again
    bool reverted = false;
    for (int i = 0; i < 40; ++i) {
        QThread::msleep(100);
        if (affOf(probe) == QStringLiteral("2-5")) { reverted = true; break; }
    }
    check(reverted, "rule reclaims the process after the override expires");

    mon.stop(); mon.wait(3000);
    kill(probe, SIGKILL); int st = 0; waitpid(probe, &st, 0);
    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
