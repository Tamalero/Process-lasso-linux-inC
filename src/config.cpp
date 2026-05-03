#include "config.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace Config {

static QString configPath()
{
    const QString dir = QDir::homePath() + QStringLiteral("/.config/process-lasso");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/config.json");
}

QJsonObject defaultConfig()
{
    QJsonObject probalance;
    probalance[QStringLiteral("enabled")]                  = true;
    probalance[QStringLiteral("cpu_threshold_percent")]    = 85.0;
    probalance[QStringLiteral("consecutive_seconds")]      = 3;
    probalance[QStringLiteral("nice_adjustment")]          = 10;
    probalance[QStringLiteral("nice_floor")]               = 15;
    probalance[QStringLiteral("restore_threshold_percent")]= 40.0;
    probalance[QStringLiteral("restore_hysteresis_seconds")]= 5;
    probalance[QStringLiteral("exempt_patterns")] = QJsonArray{
        QStringLiteral("kwin"), QStringLiteral("plasmashell"),
        QStringLiteral("systemd"), QStringLiteral("kthreadd"),
        QStringLiteral("Xorg"), QStringLiteral("xwayland")
    };

    QJsonObject monitor;
    monitor[QStringLiteral("display_refresh_interval_ms")] = 2000;
    monitor[QStringLiteral("rule_enforce_interval_ms")]    = 500;

    QJsonObject cpu;
    cpu[QStringLiteral("default_affinity")] = QJsonValue::Null;

    QJsonObject ui;
    ui[QStringLiteral("start_minimized")]  = false;
    ui[QStringLiteral("use_system_theme")] = false;
    ui[QStringLiteral("opacity")]          = 100;

    QJsonObject root;
    root[QStringLiteral("version")]     = 1;
    root[QStringLiteral("rules")]       = QJsonArray{};
    root[QStringLiteral("cpu")]         = cpu;
    root[QStringLiteral("probalance")]  = probalance;
    root[QStringLiteral("monitor")]     = monitor;
    root[QStringLiteral("ui")]          = ui;
    root[QStringLiteral("gaming_mode")] = QJsonObject{{ QStringLiteral("profiles"), QJsonObject{} }};
    return root;
}

QJsonObject deepMerge(const QJsonObject &base, const QJsonObject &override)
{
    QJsonObject result = base;
    for (auto it = override.constBegin(); it != override.constEnd(); ++it) {
        if (base.contains(it.key()) && base[it.key()].isObject() && it.value().isObject())
            result[it.key()] = deepMerge(base[it.key()].toObject(), it.value().toObject());
        else
            result[it.key()] = it.value();
    }
    return result;
}

QJsonObject load()
{
    const QString path = configPath();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return deepMerge(defaultConfig(), doc.object());
    }
    return defaultConfig();
}

void save(const QJsonObject &config)
{
    const QString path = configPath();
    const QString tmp  = path + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(config).toJson(QJsonDocument::Indented));
    f.close();
    QFile::remove(path);
    QFile::rename(tmp, path);
}

} // namespace Config
