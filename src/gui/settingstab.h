#pragma once
#include <QWidget>
#include <QCheckBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>

class SettingsTab : public QWidget {
    Q_OBJECT
public:
    explicit SettingsTab(const QJsonObject &config, QWidget *parent = nullptr);
    void updateConfig(const QJsonObject &config);

signals:
    void settingsChanged(QJsonObject config);

private:
    QJsonObject  m_config;
    QCheckBox   *m_defaultAffinityCb  = nullptr;
    QLineEdit   *m_defaultAffinityEdit = nullptr;
    QSpinBox    *m_ruleInterval   = nullptr;
    QSpinBox    *m_displayInterval = nullptr;
    QCheckBox   *m_systemThemeCb  = nullptr;
    QCheckBox   *m_showTempsCb    = nullptr;
    QSlider     *m_opacitySlider  = nullptr;
    QLabel      *m_opacityLabel   = nullptr;
    QCheckBox   *m_autostartCb    = nullptr;

    void loadConfig();
    void pickAffinity();
    void setQuick(const QString &val);
    void applyCpu();
    void applyMonitor();
    void applyAutostart();
};
