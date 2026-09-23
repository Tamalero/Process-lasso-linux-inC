// probalance-harness.cpp — standalone checks for the ProBalance state machine
// and the exempt-list rules. Not part of the CMake build: ProBalance is a plain
// class with no QObject and no /proc access, so it compiles on its own.
//
// Build and run:
//   g++ -std=c++17 -fPIC -pie -I../src $(pkg-config --cflags Qt6Core) \
//       probalance-harness.cpp ../src/probalance.cpp \
//       $(pkg-config --libs Qt6Core) -o probalance-harness && ./probalance-harness
//
// -fPIC -pie is required, or linking against Qt6 fails with
// "copy relocation against non-copyable protected symbol QString::_empty".
//
// Why this exists: launching the real app to test a change enforces the saved
// rules against every PID on the live desktop. Never do that just to verify
// ProBalance logic — extend this instead.
//
// Utils::setNice() is stubbed below, so nothing on the live desktop is touched.

#include "probalance.h"
#include "utils.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <cstdio>

bool gVerbose = false;

// --- stub out the only Utils entry point ProBalance uses ---------------------
static QHash<int,int> g_nice;          // pid -> nice as "the kernel" sees it
namespace Utils {
bool setNice(int pid, int nice) { g_nice[pid] = nice; return true; }
}

static ProcessInfo mk(int pid, const char *name, double cpu, int nice)
{
    ProcessInfo p;
    p.pid = pid; p.name = QString::fromLatin1(name);
    p.cpuPercent = cpu; p.nice = nice; p.memRss = 0;
    return p;
}

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QJsonObject cfg;
    cfg["enabled"]                    = true;
    cfg["cpu_threshold_percent"]      = 85.0;
    cfg["consecutive_seconds"]        = 3.0;
    cfg["nice_adjustment"]            = 10;
    cfg["nice_floor"]                 = 15;
    cfg["restore_threshold_percent"]  = 40.0;
    cfg["restore_hysteresis_seconds"] = 5.0;
    cfg["exempt_patterns"]            = QJsonArray{ QStringLiteral("kwin") };

    ProBalance pb(cfg, [](const QString &m){ printf("      log: %s\n", qPrintable(m)); });

    const int PID = 4242;

    // 1. A hot process gets throttled after consecutive_seconds.
    for (int i = 0; i < 4; ++i)
        pb.tick({ mk(PID, "firefox", 95.0, 0) }, 1.0);
    check(pb.throttledPids().contains(PID), "hot process is throttled");
    check(g_nice.value(PID) == 10,          "throttled nice is 0 + 10");

    // 2. Exempting it while it is still hot must undo the throttle.
    //    (Before the fix, tick() just skipped it and it stayed niced forever.)
    QJsonObject cfg2 = cfg;
    cfg2["exempt_patterns"] = QJsonArray{ QStringLiteral("kwin"), QStringLiteral("firefox") };
    pb.updateConfig(cfg2);
    pb.tick({ mk(PID, "firefox", 95.0, 10) }, 1.0);
    check(!pb.throttledPids().contains(PID), "exempted process leaves the throttled set");
    check(g_nice.value(PID) == 0,            "exempted process is reniced back to 0");

    // 3. It stays exempt: still hot, still untouched.
    g_nice[PID] = 0;
    for (int i = 0; i < 6; ++i)
        pb.tick({ mk(PID, "firefox", 99.0, 0) }, 1.0);
    check(!pb.throttledPids().contains(PID), "exempt process is never re-throttled");
    check(g_nice.value(PID) == 0,            "exempt process nice untouched while hot");

    // 4. Pattern matching is substring + case-insensitive, as the tab claims:
    //    'kwin' must cover 'KWin_wayland' but not an unrelated process.
    const int KWIN = 7, CHROME = 8;
    for (int i = 0; i < 6; ++i)
        pb.tick({ mk(KWIN, "KWin_wayland", 99.0, 0), mk(CHROME, "chromium", 99.0, 0) }, 1.0);
    check(!pb.throttledPids().contains(KWIN),  "pattern 'kwin' matches 'KWin_wayland'");
    check(pb.throttledPids().contains(CHROME), "unrelated process is not exempt");

    // 5. Removing the pattern makes it throttleable again.
    pb.updateConfig(cfg);
    for (int i = 0; i < 4; ++i)
        pb.tick({ mk(PID, "firefox", 95.0, 0) }, 1.0);
    check(pb.throttledPids().contains(PID), "un-exempted process throttles again");

    // 6. Per-pid exemption (rule-based path) restores a throttled process too.
    pb.tick({ mk(PID, "firefox", 95.0, 10) }, 1.0, QSet<int>{ PID });
    check(!pb.throttledPids().contains(PID), "pid-exempt process leaves throttled set");
    check(g_nice.value(PID) == 0,            "pid-exempt process is reniced back to 0");

    // ── 7. The shared matching rule ────────────────────────────────────────
    const QStringList pats{ QStringLiteral("kwin"), QStringLiteral("Xorg") };
    check(ProBalance::nameMatchesAny("KWin_wayland", pats), "substring, case-insensitive");
    check(ProBalance::nameMatchesAny("xorg", pats),         "pattern case is ignored too");
    check(!ProBalance::nameMatchesAny("chromium", pats),    "non-matching name");
    check(!ProBalance::nameMatches("firefox", QString()),   "empty pattern matches nothing");
    check(!ProBalance::nameMatchesAny("firefox", { QStringLiteral("") }),
          "an empty list entry does not exempt the whole machine");

    // ── 8. toggleExemptPattern: add ────────────────────────────────────────
    {
        QStringList list{ QStringLiteral("kwin") };
        QStringList removed;
        check(ProBalance::toggleExemptPattern(list, "firefox", true, removed),
              "adding a new name reports a change");
        check(list == QStringList({ QStringLiteral("kwin"), QStringLiteral("firefox") }),
              "the name is appended verbatim");

        removed.clear();
        check(!ProBalance::toggleExemptPattern(list, "firefox", true, removed),
              "adding an already-covered name is a no-op");
        check(list.size() == 2, "…and does not duplicate the entry");

        removed.clear();
        check(!ProBalance::toggleExemptPattern(list, "KWin_wayland", true, removed),
              "a broader existing pattern already covers the name");
        check(list.size() == 2, "…so nothing is added");

        removed.clear();
        check(!ProBalance::toggleExemptPattern(list, QString(), true, removed),
              "an empty name is rejected");
    }

    // ── 9. toggleExemptPattern: remove ─────────────────────────────────────
    {
        // "fire" covers firefox without being its name — removing only an exact
        // match would leave the process exempt while the menu unticked itself.
        QStringList list{ QStringLiteral("kwin"), QStringLiteral("fire"),
                          QStringLiteral("firefox") };
        QStringList removed;
        check(ProBalance::toggleExemptPattern(list, "firefox", false, removed),
              "removing reports a change");
        check(removed == QStringList({ QStringLiteral("fire"), QStringLiteral("firefox") }),
              "every pattern matching the name is removed");
        check(list == QStringList({ QStringLiteral("kwin") }), "unrelated patterns survive");
        check(!ProBalance::nameMatchesAny("firefox", list),
              "the process really is no longer exempt");

        removed.clear();
        check(!ProBalance::toggleExemptPattern(list, "firefox", false, removed),
              "removing what is not there is a no-op");
    }

    // ── 10. Session list behaves identically (same helper, different owner) ─
    {
        QStringList session, removed;
        ProBalance::toggleExemptPattern(session, "obs", true, removed);
        check(ProBalance::nameMatchesAny("obs", session), "session list exempts by name");
        check(ProBalance::nameMatchesAny("obs-ffmpeg-mux", session),
              "session pattern covers related process names, not just one pid");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
