#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QListWidget>
#include <QLineEdit>

class ProBalanceTab : public QWidget {
    Q_OBJECT
public:
    explicit ProBalanceTab(const QJsonObject &cfg, QWidget *parent = nullptr);
    QJsonObject getConfig() const;

signals:
    void settingsChanged(QJsonObject cfg);

private:
    QCheckBox      *m_enabledCb   = nullptr;
    QDoubleSpinBox *m_cpuThresh   = nullptr;
    QSpinBox       *m_consecSecs  = nullptr;
    QSpinBox       *m_niceAdj     = nullptr;
    QSpinBox       *m_niceFloor   = nullptr;
    QDoubleSpinBox *m_restoreThresh = nullptr;
    QSpinBox       *m_restoreHyst = nullptr;
    QListWidget    *m_exemptList  = nullptr;
    QLineEdit      *m_exemptEdit  = nullptr;

    void addExempt();
    void delExempt();
    void apply();
};
