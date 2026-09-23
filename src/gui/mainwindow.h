#pragma once
#include "../config.h"
#include "../runstate.h"
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
    QWidget          *m_safeBanner   = nullptr;
    RunStateInfo      m_runState;
    bool              m_safeMode     = false;
    // Session-only ProBalance exemptions: deliberately not in m_config, so
    // saveConfig() cannot leak them into config.json.
    QStringList       m_pbSessionExempt;
    // Processes tab → "Overwrite matching rules". Persisted so the choice
    // survives a restart; see config key ui.overwrite_matching_rules.
    QCheckBox        *m_overwriteRulesCb = nullptr;
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
    void recoverFromUncleanShutdown();
    void exitSafeMode();

    void onSnapshot(const QList<ProcessInfo> &snapshot);
    void appendLog(const QString &msg);
    void onCpuForTray(const QList<double> &percpu);
    void onSensors(const SensorSnapshot &sensors);
    void onRulesChanged();
    void onManualChange(ManualChange change);
    void onAffinityEscalationNeeded(QString ruleId, QString ruleName, int pid,
                                    QString procName, QString cpulist);
    void onRuleAddFromTable(Rule rule);
    void onPbSettingsChanged(QJsonObject pbCfg);
    void onPbExemptPermanentToggle(const QString &name, bool exempt);
    void onPbExemptSessionToggle(const QString &name, bool exempt);
    // Patterns from config["probalance"]["exempt_patterns"], read on the GUI
    // thread only — never through ProBalance, which belongs to the monitor.
    QStringList pbExemptPatterns() const;
    // Pushes both lists to the table (display + menu ticks) and the session list
    // to the monitor. Call after anything that changes either list.
    void refreshPbExemptPatterns();
    void onResetRequested();
    void onGamingModeChanged(bool active, bool elevateNice);
    void onSettingsChanged(QJsonObject updatedConfig);
    void updateTrayGamingAction(bool active, bool);
};
