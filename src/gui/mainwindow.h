#pragma once
#include "../config.h"
#include "../processmonitor.h"
#include "../probalance.h"
#include "../ruleengine.h"
#include "cpubarwidget.h"
#include "gamingmodetab.h"
#include "probalancetab.h"
#include "processtablewidget.h"
#include "ruleseditor.h"
#include "companionwidget.h"
#include "settingstab.h"
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTextEdit>
#include <QCheckBox>
#include <QLabel>

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QApplication *app, QWidget *parent = nullptr);
    ~MainWindow() override;

public slots:
    // Clean shutdown: unparks CPUs, saves config, stops the monitor.
    // Also reached from the SIGTERM/SIGINT/SIGHUP handler in main.cpp.
    void quitApp();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QApplication     *m_app;
    QJsonObject       m_config;
    RuleEngine        m_ruleEngine;
    ProBalance       *m_proBalance = nullptr;
    ProcessMonitor   *m_monitor    = nullptr;
    QSystemTrayIcon  *m_tray             = nullptr;
    QAction          *m_trayGamingAction    = nullptr;
    QAction          *m_trayCompanionAction = nullptr;
    CompanionWidget  *m_companion       = nullptr;

    QTabWidget       *m_tabs         = nullptr;
    CpuHistoryWidget *m_cpuHistory   = nullptr;
    CpuBarsWidget    *m_cpuBars      = nullptr;
    ProcessTableWidget *m_procTable  = nullptr;
    RulesEditor      *m_rulesEditor  = nullptr;
    ProBalanceTab    *m_pbTab        = nullptr;
    GamingModeTab    *m_gamingTab    = nullptr;
    SettingsTab      *m_settingsTab  = nullptr;
    QTextEdit        *m_logEdit      = nullptr;
    QCheckBox        *m_logAutoScroll = nullptr;
    QLabel           *m_tempStatus   = nullptr;
    double            m_lastCpuTempC = 0.0;
    bool              m_haveCpuTemp  = false;

    void buildUi();
    void buildTray();
    void startMonitor();
    void saveConfig();
    void applyTheme();
    void applyTemperatureSetting();
    void toggleWindow();
    void restoreParkedCpus();

    void onSnapshot(const QList<ProcessInfo> &snapshot);
    void appendLog(const QString &msg);
    void onCpuForTray(const QList<double> &percpu);
    void onSensors(const SensorSnapshot &sensors);
    void onRulesChanged();
    void onAffinityManualChange(int pid);
    void onRuleAddFromTable(Rule rule);
    void onPbSettingsChanged(QJsonObject pbCfg);
    void onPbExemptToggle(int pid, bool exempt);
    void onResetRequested();
    void onGamingModeChanged(bool active, bool elevateNice);
    void onSettingsChanged(QJsonObject updatedConfig);
    void updateTrayGamingAction(bool active, bool);
};
