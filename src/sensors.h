#pragma once
#include <QHash>
#include <QList>
#include <QString>

// One named temperature reading (a DIMM, a package, …).
struct SensorReading {
    QString label;
    double  celsius = 0.0;
};

// Everything one hwmon sweep produced.
struct SensorSnapshot {
    bool                 hasPackage = false;
    double               packageC   = 0.0;  // CPU package / Tdie
    QHash<int, double>   perCpu;            // logical CPU index → °C
    QList<SensorReading> memory;            // DDR4 jc42 / DDR5 spd5118 DIMMs

    // Hottest CPU reading available, package first, per-core as fallback.
    // Returns false when the machine exposes no CPU temperature at all.
    bool   cpuMax(double &out) const;
    // Hottest DIMM. False when no memory sensor exists.
    bool   memoryMax(double &out) const;
};

namespace Sensors {

// Reads every discovered sensor. Sensor paths and the CPU topology map are
// cached after the first call; discovery re-runs automatically if a cached
// path disappears. Called from the monitor thread, never the GUI thread.
SensorSnapshot read();

// True if the last discovery pass found any usable temperature sensor.
bool available();

} // namespace Sensors
