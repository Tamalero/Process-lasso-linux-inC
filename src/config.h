#pragma once
#include <QJsonObject>
#include <QString>

namespace Config {

QJsonObject defaultConfig();
QJsonObject load();
void        save(const QJsonObject &config);

// Deep-merge override into base (like Python's _deep_merge)
QJsonObject deepMerge(const QJsonObject &base, const QJsonObject &override);

} // namespace Config
