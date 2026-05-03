#pragma once
#include "../cputopology.h"
#include <QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QHash>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>

class GamingModeTab : public QWidget {
    Q_OBJECT
public:
    explicit GamingModeTab(const QJsonObject &config, QWidget *parent = nullptr);
    bool isParked() const { return m_parked; }

signals:
    void resetRequested();
    void logMessage(QString msg);
    void gamingModeChanged(bool active, bool elevateNice);
    void configChanged(QJsonObject cfg);

public slots:
    void toggleGamingMode();

private:
    QJsonObject m_config;
    CpuTopology m_topo;
    bool        m_parked = false;
    bool        m_pendingEnableAfterUnpark = false;

    QLabel      *m_topoLabel       = nullptr;
    QLabel      *m_helperStatus    = nullptr;
    QLabel      *m_cpuStatusLabel  = nullptr;
    QPushButton *m_parkBtn         = nullptr;
    QCheckBox   *m_niceCb          = nullptr;
    QWidget     *m_coreSelGroup    = nullptr;
    QHash<int, QCheckBox *> m_preferredCbs;
    QSet<int>   m_smtSiblings;

    // Launcher
    QLineEdit   *m_gameNameEdit   = nullptr;
    QLineEdit   *m_cmdEdit        = nullptr;
    QPushButton *m_launchBtn      = nullptr;
    QPushButton *m_killGameBtn    = nullptr;
    QCheckBox   *m_autoRestoreCb  = nullptr;
    QLabel      *m_watchStatusLabel = nullptr;
    QComboBox   *m_profileCombo   = nullptr;
    QTextEdit   *m_log            = nullptr;

    // Game watcher
    QString  m_launchedName;
    int      m_launchedPid  = -1;
    QString  m_watchPhase   = QStringLiteral("idle");
    QTimer  *m_watchTimer   = nullptr;
    QProcess *m_launchProc  = nullptr;

    void buildUi();
    void detectTopology();
    void updateHelperStatus();
    void updateCpuStatus();
    void installHelper();
    void enableGamingMode();
    void disableGamingMode();
    void onParkDone(bool ok, const QString &msg);
    void onUnparkDone(bool ok);
    void resetAll();
    void selectPreferred(const QString &mode);

    // Profiles
    void refreshProfilesCombo(const QString &select = {});
    void saveProfile();
    void loadProfile();
    void deleteProfile();

    // Launcher
    void onGameFieldsChanged();
    void pickSteamGame();
    void pickLutrisGame();
    void launchWithGamingMode();
    void pollGameProcess();
    void stopWatch(bool restore);
    void killLaunched();
    bool procNameMatches(const QString &gameName, int pid);

    void appendLog(const QString &msg);
};
