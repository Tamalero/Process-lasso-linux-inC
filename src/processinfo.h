#pragma once
#include <QString>

struct ProcessInfo {
    int     pid        = 0;
    QString name;
    double  cpuPercent = 0.0;
    qint64  memRss     = 0;   // bytes
    int     nice       = 0;
    QString affinity;         // "0-7,16-23"
    QString ionice;           // "class/level"
    QString cmdline;          // space-joined argv
};
