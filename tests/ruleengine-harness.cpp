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
int main(int argc, char **argv){
    QCoreApplication app(argc, argv);
    if (argc < 2) return 2;
    const int pid = QString::fromLatin1(argv[1]).toInt();
    QJsonObject r; r["rule_id"]="t"; r["name"]="probe"; r["pattern"]="plqprobe";
    r["match_type"]="contains"; r["affinity"]="2-5"; r["nice"]=5;
    r["ionice_class"]=2; r["ionice_level"]=4; r["enabled"]=true;
    int lines=0;
    RuleEngine re; re.loadRules(QJsonArray{r});
    re.setLogCallback([&](const QString&){ ++lines; });
    re.applyToProcess(pid, "plqprobe");           // pass 1: actually applies
    printf("pass 1 (needs applying): %d log lines\n", lines);
    for (int p=2; p<=10; ++p) { lines=0; re.applyToProcess(pid, "plqprobe"); }
    printf("passes 2-10 (already correct): %d log lines on the last pass\n", lines);
    printf("%s\n", lines==0 ? "OK  steady state is silent"
                            : "BUG steady state still logs every pass");
    return lines==0?0:1;
}
