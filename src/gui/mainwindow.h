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

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QApplication *app, QWidget *parent = nullptr);
    ~MainWindow() override;

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

    void buildUi();
    void buildTray();
    void startMonitor();
    void saveConfig();
    void applyTheme();
    void toggleWindow();
    void quitApp();

    void onSnapshot(const QList<ProcessInfo> &snapshot);
    void appendLog(const QString &msg);
    void onCpuForTray(const QList<double> &percpu);
    void onRulesChanged();
    void onAffinityManualChange(int pid);
    void onRuleAddFromTable(Rule rule);
    void onPbSettingsChanged(QJsonObject pbCfg);
    void onResetRequested();
    void onGamingModeChanged(bool active, bool elevateNice);
    void onSettingsChanged(QJsonObject updatedConfig);
    void updateTrayGamingAction(bool active, bool);
};
