#include "gamingmodetab.h"
#include "dialogs.h"
#include "../cpupark.h"
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QThread>
#include <algorithm>
#include <csignal>

GamingModeTab::GamingModeTab(const QJsonObject &config, QWidget *parent)
    : QWidget(parent), m_config(config)
{
    buildUi();
    detectTopology();
}

void GamingModeTab::buildUi()
{
    auto *outer  = new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *inner  = new QWidget;
    scroll->setWidget(inner);
    outer->addWidget(scroll);
    auto *layout = new QVBoxLayout(inner);

    // Topology info
    auto *topoGroup = new QGroupBox(QStringLiteral("CPU Topology"), inner);
    auto *topoLayout = new QVBoxLayout(topoGroup);
    m_topoLabel = new QLabel(QStringLiteral("Detecting…"), topoGroup);
    m_topoLabel->setWordWrap(true);
    topoLayout->addWidget(m_topoLabel);
    layout->addWidget(topoGroup);

    // Gaming Mode
    auto *parkGroup = new QGroupBox(QStringLiteral("Gaming Mode — CPU Parking"), inner);
    auto *parkLayout = new QVBoxLayout(parkGroup);
    auto *desc = new QLabel(QStringLiteral(
        "Parks (takes offline) non-preferred CPUs so the game initialises its\n"
        "thread pool against the correct CPU count — no frametime jitter.\n\n"
        "  AMD X3D  → parks non-V-Cache CCD (smaller L3)\n"
        "  Intel Hybrid → parks E-cores (lower max freq)\n"
        "  Uniform CPU → parking disabled (no asymmetry)"), parkGroup);
    desc->setWordWrap(true);
    parkLayout->addWidget(desc);

    // Core selection group (shown after topology detection)
    m_coreSelGroup = new QGroupBox(QStringLiteral("Preferred CCD — Active Cores in Gaming Mode"), parkGroup);
    auto *coreSelLayout = new QVBoxLayout(m_coreSelGroup);
    auto *coreSelInfo = new QLabel(QStringLiteral(
        "All preferred-CCD CPUs are kept online by default.\n"
        "Uncheck any CPU to park it along with the non-preferred CCD.\n"
        "Use 'No SMT' to park the hyperthread siblings."), m_coreSelGroup);
    coreSelInfo->setWordWrap(true);
    coreSelLayout->addWidget(coreSelInfo);
    auto *gridWidget = new QWidget(m_coreSelGroup);
    auto *gridLayout = new QGridLayout(gridWidget);
    gridLayout->setObjectName(QStringLiteral("preferredGrid"));
    gridLayout->setHorizontalSpacing(8); gridLayout->setVerticalSpacing(3);
    coreSelLayout->addWidget(gridWidget);
    auto *quickRow = new QHBoxLayout;
    auto *allBtn    = new QPushButton(QStringLiteral("All"), m_coreSelGroup);
    auto *noSmtBtn  = new QPushButton(QStringLiteral("No SMT (physical only)"), m_coreSelGroup);
    auto *noneBtn   = new QPushButton(QStringLiteral("None"), m_coreSelGroup);
    connect(allBtn,   &QPushButton::clicked, this, [this]{ selectPreferred(QStringLiteral("all")); });
    connect(noSmtBtn, &QPushButton::clicked, this, [this]{ selectPreferred(QStringLiteral("no_smt")); });
    connect(noneBtn,  &QPushButton::clicked, this, [this]{ selectPreferred(QStringLiteral("none")); });
    quickRow->addWidget(allBtn); quickRow->addWidget(noSmtBtn); quickRow->addWidget(noneBtn);
    quickRow->addStretch();
    coreSelLayout->addLayout(quickRow);
    m_coreSelGroup->setVisible(false);
    parkLayout->addWidget(m_coreSelGroup);

    auto *helperFrame = new QFrame(parkGroup);
    helperFrame->setFrameShape(QFrame::StyledPanel);
    auto *helperLayout = new QHBoxLayout(helperFrame);
    m_helperStatus = new QLabel(helperFrame);
    helperLayout->addWidget(m_helperStatus);
    auto *installBtn = new QPushButton(QStringLiteral("Install / Update Helper (root)"), helperFrame);
    connect(installBtn, &QPushButton::clicked, this, &GamingModeTab::installHelper);
    helperLayout->addWidget(installBtn);
    helperLayout->addStretch();
    parkLayout->addWidget(helperFrame);

    m_niceCb = new QCheckBox(QStringLiteral("Elevate game priority (nice -1)"), parkGroup);
    m_niceCb->setChecked(true);
    parkLayout->addWidget(m_niceCb);

    auto *btnRow = new QHBoxLayout;
    m_parkBtn = new QPushButton(QStringLiteral("▶  Enable Gaming Mode (Park non-preferred CPUs)"), parkGroup);
    m_parkBtn->setMinimumHeight(40);
    QFont f; f.setBold(true); m_parkBtn->setFont(f);
    connect(m_parkBtn, &QPushButton::clicked, this, &GamingModeTab::toggleGamingMode);
    btnRow->addWidget(m_parkBtn);
    parkLayout->addLayout(btnRow);

    m_cpuStatusLabel = new QLabel(parkGroup);
    m_cpuStatusLabel->setWordWrap(true);
    parkLayout->addWidget(m_cpuStatusLabel);
    layout->addWidget(parkGroup);

    // Reset All
    auto *resetGroup = new QGroupBox(QStringLiteral("Reset All Changes"), inner);
    auto *resetLayout = new QVBoxLayout(resetGroup);
    auto *resetDesc = new QLabel(QStringLiteral(
        "Restores all per-process CPU affinities that Process Lasso has changed\n"
        "back to their original state, and unparks any parked CPUs."), resetGroup);
    resetDesc->setWordWrap(true);
    resetLayout->addWidget(resetDesc);
    auto *resetBtn = new QPushButton(QStringLiteral("↩  Reset All Changes"), resetGroup);
    resetBtn->setMinimumHeight(36);
    connect(resetBtn, &QPushButton::clicked, this, &GamingModeTab::resetAll);
    resetLayout->addWidget(resetBtn);
    layout->addWidget(resetGroup);

    // Launcher
    auto *launcherGroup = new QGroupBox(QStringLiteral("Game Launcher"), inner);
    auto *launcherLayout = new QVBoxLayout(launcherGroup);
    // Profile row
    auto *profileRow = new QHBoxLayout;
    profileRow->addWidget(new QLabel(QStringLiteral("Profile:"), launcherGroup));
    m_profileCombo = new QComboBox(launcherGroup);
    m_profileCombo->setMinimumWidth(160);
    profileRow->addWidget(m_profileCombo);
    auto *saveProfileBtn = new QPushButton(QStringLiteral("Save"), launcherGroup);
    auto *delProfileBtn  = new QPushButton(QStringLiteral("Delete"), launcherGroup);
    connect(saveProfileBtn, &QPushButton::clicked, this, &GamingModeTab::saveProfile);
    connect(delProfileBtn,  &QPushButton::clicked, this, &GamingModeTab::deleteProfile);
    profileRow->addWidget(saveProfileBtn); profileRow->addWidget(delProfileBtn);
    profileRow->addStretch();
    launcherLayout->addLayout(profileRow);
    // Game name
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(QStringLiteral("Game:"), launcherGroup));
    m_gameNameEdit = new QLineEdit(launcherGroup);
    m_gameNameEdit->setPlaceholderText(QStringLiteral("Game name (used to detect running process)"));
    connect(m_gameNameEdit, &QLineEdit::textChanged, this, &GamingModeTab::onGameFieldsChanged);
    nameRow->addWidget(m_gameNameEdit);
    auto *steamBtn  = new QPushButton(QStringLiteral("Steam…"),  launcherGroup);
    auto *lutrisBtn = new QPushButton(QStringLiteral("Lutris…"), launcherGroup);
    connect(steamBtn,  &QPushButton::clicked, this, &GamingModeTab::pickSteamGame);
    connect(lutrisBtn, &QPushButton::clicked, this, &GamingModeTab::pickLutrisGame);
    nameRow->addWidget(steamBtn); nameRow->addWidget(lutrisBtn);
    launcherLayout->addLayout(nameRow);
    // Command
    auto *cmdRow = new QHBoxLayout;
    cmdRow->addWidget(new QLabel(QStringLiteral("Command:"), launcherGroup));
    m_cmdEdit = new QLineEdit(launcherGroup);
    m_cmdEdit->setPlaceholderText(QStringLiteral("e.g.  steam -applaunch 238960   or   /path/to/game"));
    connect(m_cmdEdit, &QLineEdit::textChanged, this, &GamingModeTab::onGameFieldsChanged);
    cmdRow->addWidget(m_cmdEdit);
    launcherLayout->addLayout(cmdRow);
    // Launch row
    auto *launchRow = new QHBoxLayout;
    m_launchBtn = new QPushButton(QStringLiteral("▶  Launch"), launcherGroup);
    m_launchBtn->setEnabled(false); m_launchBtn->setMinimumHeight(36);
    QFont f2; f2.setBold(true); m_launchBtn->setFont(f2);
    connect(m_launchBtn, &QPushButton::clicked, this, &GamingModeTab::launchWithGamingMode);
    launchRow->addWidget(m_launchBtn);
    m_autoRestoreCb = new QCheckBox(QStringLiteral("Auto-disable Gaming Mode when game exits"), launcherGroup);
    m_autoRestoreCb->setChecked(true);
    launchRow->addWidget(m_autoRestoreCb);
    launchRow->addStretch();
    launcherLayout->addLayout(launchRow);
    // Kill row
    auto *killRow = new QHBoxLayout;
    m_killGameBtn = new QPushButton(QStringLiteral("⏹ Kill Game"), launcherGroup);
    m_killGameBtn->setEnabled(false);
    connect(m_killGameBtn, &QPushButton::clicked, this, &GamingModeTab::killLaunched);
    killRow->addWidget(m_killGameBtn);
    m_watchStatusLabel = new QLabel(launcherGroup);
    m_watchStatusLabel->setStyleSheet(QStringLiteral("color: #a6e3a1;"));
    killRow->addWidget(m_watchStatusLabel); killRow->addStretch();
    launcherLayout->addLayout(killRow);
    layout->addWidget(launcherGroup);

    // Log
    m_log = new QTextEdit(inner);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(140);
    m_log->setLineWrapMode(QTextEdit::NoWrap);
    layout->addWidget(m_log);
    layout->addStretch();

    refreshProfilesCombo();
    connect(m_profileCombo, &QComboBox::currentTextChanged, this, &GamingModeTab::loadProfile);
    updateHelperStatus();
    updateCpuStatus();
}

void GamingModeTab::detectTopology()
{
    m_topo = ::detectTopology();
    m_topoLabel->setText(m_topo.description);
    const bool hasAsym = m_topo.hasAsymmetry() && CpuPark::isHelperInstalled();
    m_parkBtn->setEnabled(hasAsym);
    if (!m_topo.hasAsymmetry()) {
        m_parkBtn->setText(QStringLiteral("Gaming Mode unavailable (uniform CPU topology)"));
        m_coreSelGroup->setVisible(false);
        return;
    }
    m_coreSelGroup->setVisible(true);
    const auto preferred = m_topo.preferred.values();
    m_smtSiblings = getSmtSiblingsOf(m_topo.preferred);
    m_preferredCbs.clear();
    // Rebuild the grid
    auto *gridLayout = qobject_cast<QGridLayout *>(
        m_coreSelGroup->findChild<QWidget *>()->layout());
    while (gridLayout->count()) {
        auto *item = gridLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    QList<int> sorted = preferred;
    std::sort(sorted.begin(), sorted.end());
    const auto offline = getOfflineCpuSet();
    for (int idx = 0; idx < sorted.size(); ++idx) {
        const int cpu = sorted[idx];
        const bool isSmt = m_smtSiblings.contains(cpu);
        auto *cb = new QCheckBox(
            QStringLiteral("CPU %1%2").arg(cpu).arg(isSmt ? QStringLiteral(" (HT)") : QString{}),
            m_coreSelGroup);
        cb->setChecked(!offline.contains(cpu));
        gridLayout->addWidget(cb, idx / 8, idx % 8);
        m_preferredCbs[cpu] = cb;
    }
    if (!offline.isEmpty()) {
        m_parked = true;
        m_parkBtn->setText(QStringLiteral("⏹  Disable Gaming Mode (Unpark CPUs)"));
        m_parkBtn->setStyleSheet(QStringLiteral(
            "background-color: #1e4a2a; color: #a6e3a1; border: 1px solid #a6e3a1;"));
        emit gamingModeChanged(true, m_niceCb->isChecked());
    }
}

void GamingModeTab::updateHelperStatus()
{
    if (CpuPark::isHelperCurrent() && CpuPark::isSudoersInstalled()) {
        m_helperStatus->setText(QStringLiteral("✓ Helper installed — parking + nice -1 available"));
        m_helperStatus->setStyleSheet(QStringLiteral("color: #a6e3a1;"));
    } else if (CpuPark::isHelperInstalled() && CpuPark::isSudoersInstalled()) {
        m_helperStatus->setText(QStringLiteral("⚠ Helper needs update — click 'Install / Update Helper'"));
        m_helperStatus->setStyleSheet(QStringLiteral("color: #f9e2af;"));
    } else {
        m_helperStatus->setText(QStringLiteral("✗ Helper not installed — click 'Install / Update Helper' to enable parking"));
        m_helperStatus->setStyleSheet(QStringLiteral("color: #f38ba8;"));
    }
}

void GamingModeTab::updateCpuStatus()
{
    const auto online  = getOnlineCpuSet();
    const auto offline = getOfflineCpuSet();
    if (!offline.isEmpty()) {
        auto sortedOnline = online.values(); std::sort(sortedOnline.begin(), sortedOnline.end());
        auto sortedOff    = offline.values(); std::sort(sortedOff.begin(), sortedOff.end());
        QStringList on, off;
        for (int c : sortedOnline) on  << QString::number(c);
        for (int c : sortedOff)    off << QString::number(c);
        m_cpuStatusLabel->setText(QStringLiteral("Online: [%1]  |  Offline (parked): [%2]")
            .arg(on.join(','), off.join(',')));
        m_cpuStatusLabel->setStyleSheet(QStringLiteral("color: orange;"));
    } else {
        auto sorted = online.values(); std::sort(sorted.begin(), sorted.end());
        QStringList nums; for (int c : sorted) nums << QString::number(c);
        m_cpuStatusLabel->setText(QStringLiteral("All CPUs online: [%1]").arg(nums.join(',')));
        m_cpuStatusLabel->setStyleSheet({});
    }
}

void GamingModeTab::installHelper()
{
    auto [ok, msg] = CpuPark::installHelper();
    appendLog(msg);
    updateHelperStatus();
    if (m_topo.hasAsymmetry()) m_parkBtn->setEnabled(ok);
    QMessageBox::information(this, QStringLiteral("Install Helper"), msg);
}

void GamingModeTab::toggleGamingMode()
{
    if (m_parked) disableGamingMode();
    else          enableGamingMode();
}

void GamingModeTab::enableGamingMode()
{
    if (!m_topo.hasAsymmetry()) return;
    if (!CpuPark::isHelperInstalled()) {
        QMessageBox::warning(this, QStringLiteral("Helper Missing"),
                             QStringLiteral("Install the privileged helper first."));
        return;
    }
    QSet<int> unchecked;
    for (auto it = m_preferredCbs.constBegin(); it != m_preferredCbs.constEnd(); ++it)
        if (!it.value()->isChecked()) unchecked.insert(it.key());
    const auto toPark = m_topo.nonPreferred | unchecked;
    appendLog(QStringLiteral("[Gaming Mode] Parking CPUs…"));
    m_parkBtn->setEnabled(false);
    // Run in background thread
    auto *thread = new QThread(this);
    connect(thread, &QThread::started, this, [this, toPark, thread]{
        QStringList logs;
        bool ok = CpuPark::parkCpus(toPark, [&logs](const QString &m){ logs << m; });
        for (const auto &l : logs) appendLog(l);
        onParkDone(ok, logs.join('\n'));
        thread->quit();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void GamingModeTab::disableGamingMode()
{
    appendLog(QStringLiteral("[Gaming Mode] Unparking all CPUs…"));
    m_parkBtn->setEnabled(false);
    auto *thread = new QThread(this);
    connect(thread, &QThread::started, this, [this, thread]{
        QStringList logs;
        bool ok = CpuPark::unParkAll([&logs](const QString &m){ logs << m; });
        for (const auto &l : logs) appendLog(l);
        onUnparkDone(ok);
        thread->quit();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void GamingModeTab::onParkDone(bool ok, const QString &)
{
    m_parked = ok;
    m_parkBtn->setEnabled(true);
    updateCpuStatus();
    if (ok) {
        m_parkBtn->setText(QStringLiteral("⏹  Disable Gaming Mode (Unpark CPUs)"));
        m_parkBtn->setStyleSheet(QStringLiteral(
            "background-color: #1e4a2a; color: #a6e3a1; border: 1px solid #a6e3a1;"));
        appendLog(QStringLiteral("[Gaming Mode] ACTIVE — non-preferred CPUs offline."));
        emit gamingModeChanged(true, m_niceCb->isChecked());
    } else {
        appendLog(QStringLiteral("[Gaming Mode] Parking failed — check log."));
    }
    emit logMessage(QStringLiteral("[Gaming Mode] %1").arg(ok ? QStringLiteral("enabled") : QStringLiteral("FAILED")));
}

void GamingModeTab::onUnparkDone(bool)
{
    m_parked = false;
    m_parkBtn->setEnabled(true);
    updateCpuStatus();
    m_parkBtn->setText(QStringLiteral("▶  Enable Gaming Mode (Park non-preferred CPUs)"));
    m_parkBtn->setStyleSheet({});
    appendLog(QStringLiteral("[Gaming Mode] Disabled — all CPUs online."));
    emit gamingModeChanged(false, false);
    emit logMessage(QStringLiteral("[Gaming Mode] disabled"));
    detectTopology();
    if (m_pendingEnableAfterUnpark) {
        m_pendingEnableAfterUnpark = false;
        enableGamingMode();
    }
}

void GamingModeTab::resetAll()
{
    if (QMessageBox::question(this, QStringLiteral("Reset All Changes"),
            QStringLiteral("This will:\n  • Restore all per-process CPU affinities\n  • Unpark any parked CPUs\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    if (m_parked) { emit gamingModeChanged(false, false); m_parked = false;
        m_parkBtn->setText(QStringLiteral("▶  Enable Gaming Mode (Park non-preferred CPUs)"));
        m_parkBtn->setStyleSheet({}); }
    if (!getOfflineCpuSet().isEmpty()) {
        appendLog(QStringLiteral("[Reset] Unparking CPUs…"));
        CpuPark::unParkAll([this](const QString &m){ appendLog(m); });
        updateCpuStatus(); detectTopology();
    }
    emit resetRequested();
}

void GamingModeTab::selectPreferred(const QString &mode)
{
    for (auto it = m_preferredCbs.constBegin(); it != m_preferredCbs.constEnd(); ++it) {
        if      (mode == QLatin1String("all"))    it.value()->setChecked(true);
        else if (mode == QLatin1String("none"))   it.value()->setChecked(false);
        else if (mode == QLatin1String("no_smt")) it.value()->setChecked(!m_smtSiblings.contains(it.key()));
    }
}

// ── Profiles ───────────────────────────────────────────────────────────────────

void GamingModeTab::refreshProfilesCombo(const QString &select)
{
    m_profileCombo->blockSignals(true);
    m_profileCombo->clear();
    const auto profiles = m_config[QStringLiteral("gaming_mode")].toObject()[QStringLiteral("profiles")].toObject();
    for (const auto &key : profiles.keys()) m_profileCombo->addItem(key);
    if (!select.isEmpty()) {
        const int idx = m_profileCombo->findText(select);
        if (idx >= 0) m_profileCombo->setCurrentIndex(idx);
    }
    m_profileCombo->blockSignals(false);
}

void GamingModeTab::saveProfile()
{
    const QString def = m_gameNameEdit->text().trimmed();
    bool ok;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save Profile"),
        QStringLiteral("Profile name:"), QLineEdit::Normal, def, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QJsonObject cpuStates;
    for (auto it = m_preferredCbs.constBegin(); it != m_preferredCbs.constEnd(); ++it)
        cpuStates[QString::number(it.key())] = it.value()->isChecked();
    QJsonObject profile;
    profile[QStringLiteral("game_name")]    = m_gameNameEdit->text().trimmed();
    profile[QStringLiteral("command")]      = m_cmdEdit->text().trimmed();
    profile[QStringLiteral("cpu_states")]   = cpuStates;
    profile[QStringLiteral("elevate_nice")] = m_niceCb->isChecked();
    auto gm = m_config[QStringLiteral("gaming_mode")].toObject();
    auto profiles = gm[QStringLiteral("profiles")].toObject();
    profiles[name.trimmed()] = profile;
    gm[QStringLiteral("profiles")] = profiles;
    m_config[QStringLiteral("gaming_mode")] = gm;
    emit configChanged(m_config);
    refreshProfilesCombo(name.trimmed());
    appendLog(QStringLiteral("[Profile] Saved '%1'").arg(name.trimmed()));
}

void GamingModeTab::loadProfile()
{
    const QString name = m_profileCombo->currentText();
    if (name.isEmpty()) return;
    const auto profile = m_config[QStringLiteral("gaming_mode")].toObject()
        [QStringLiteral("profiles")].toObject()[name].toObject();
    if (profile.isEmpty()) return;
    m_gameNameEdit->setText(profile[QStringLiteral("game_name")].toString());
    m_cmdEdit->setText(profile[QStringLiteral("command")].toString());
    if (profile.contains(QStringLiteral("elevate_nice")))
        m_niceCb->setChecked(profile[QStringLiteral("elevate_nice")].toBool(true));
    const auto cpuStates = profile[QStringLiteral("cpu_states")].toObject();
    for (auto it = m_preferredCbs.constBegin(); it != m_preferredCbs.constEnd(); ++it)
        it.value()->setChecked(cpuStates[QString::number(it.key())].toBool(true));
    appendLog(QStringLiteral("[Profile] Loaded '%1'").arg(name));
    if (m_parked) {
        appendLog(QStringLiteral("[Profile] Re-applying CPU parking…"));
        disableGamingMode();
        m_pendingEnableAfterUnpark = true;
    }
}

void GamingModeTab::deleteProfile()
{
    const QString name = m_profileCombo->currentText();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Delete Profile"),
            QStringLiteral("Delete profile '%1'?").arg(name),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    auto gm = m_config[QStringLiteral("gaming_mode")].toObject();
    auto profiles = gm[QStringLiteral("profiles")].toObject();
    profiles.remove(name);
    gm[QStringLiteral("profiles")] = profiles;
    m_config[QStringLiteral("gaming_mode")] = gm;
    emit configChanged(m_config);
    refreshProfilesCombo();
}

// ── Launcher ───────────────────────────────────────────────────────────────────

void GamingModeTab::onGameFieldsChanged()
{
    m_launchBtn->setEnabled(!m_gameNameEdit->text().trimmed().isEmpty() &&
                            !m_cmdEdit->text().trimmed().isEmpty());
}

void GamingModeTab::pickSteamGame()
{
    SteamGamePickerDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.getAppId().isEmpty()) {
        m_gameNameEdit->setText(dlg.getName());
        m_cmdEdit->setText(QStringLiteral("steam -applaunch %1").arg(dlg.getAppId()));
    }
}

void GamingModeTab::pickLutrisGame()
{
    LutrisGamePickerDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.getSlug().isEmpty()) {
        m_gameNameEdit->setText(dlg.getName());
        m_cmdEdit->setText(QStringLiteral("lutris lutris:rungame/%1").arg(dlg.getSlug()));
    }
}

void GamingModeTab::launchWithGamingMode()
{
    const QString cmd  = m_cmdEdit->text().trimmed();
    const QString name = m_gameNameEdit->text().trimmed();
    if (cmd.isEmpty() || name.isEmpty()) return;
    if (!m_parked) enableGamingMode();
    m_launchedName = name;
    m_launchedPid  = -1;
    m_watchPhase   = QStringLiteral("waiting");
    m_watchStatusLabel->setText(QStringLiteral("Waiting for game process…"));
    m_killGameBtn->setEnabled(true);
    appendLog(QStringLiteral("[Launcher] Launching '%1': %2").arg(name, cmd));
    if (m_launchProc) { m_launchProc->kill(); m_launchProc->deleteLater(); }
    m_launchProc = new QProcess(this);
    const auto parts = cmd.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    m_launchProc->setProgram(parts[0]);
    m_launchProc->setArguments(parts.mid(1));
    connect(m_launchProc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this](int code, QProcess::ExitStatus){
            appendLog(QStringLiteral("[Launcher] Launcher process exited (code %1) — watching /proc…").arg(code));
        });
    m_launchProc->start();
    if (m_watchTimer) m_watchTimer->stop();
    m_watchTimer = new QTimer(this);
    m_watchTimer->setInterval(2000);
    connect(m_watchTimer, &QTimer::timeout, this, &GamingModeTab::pollGameProcess);
    m_watchTimer->start();
}

bool GamingModeTab::procNameMatches(const QString &gameName, int pid)
{
    auto norm = [](const QString &s) {
        QString r; for (const QChar &c : s.toLower()) if (c.isLetterOrNumber()) r += c;
        return r;
    };
    const QString nameN = norm(gameName);
    QFile cf(QStringLiteral("/proc/%1/comm").arg(pid));
    if (cf.open(QIODevice::ReadOnly)) {
        const QString commN = norm(QString(cf.readAll()).trimmed());
        if (nameN.contains(commN) || commN.contains(nameN)) return true;
    }
    QFile clf(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (clf.open(QIODevice::ReadOnly))
        if (norm(QString(clf.readAll())).contains(nameN)) return true;
    return false;
}

void GamingModeTab::pollGameProcess()
{
    QList<int> pids;
    for (const auto &e : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok; int pid = e.toInt(&ok); if (ok) pids << pid;
    }
    if (m_watchPhase == QLatin1String("waiting")) {
        for (int pid : pids) {
            if (procNameMatches(m_launchedName, pid)) {
                m_launchedPid = pid;
                m_watchPhase  = QStringLiteral("running");
                m_watchStatusLabel->setText(QStringLiteral("Game running (PID %1)").arg(pid));
                appendLog(QStringLiteral("[Launcher] Game process found: PID %1").arg(pid));
                m_watchTimer->setInterval(5000);
                return;
            }
        }
    } else if (m_watchPhase == QLatin1String("running")) {
        if (!pids.contains(m_launchedPid)) {
            // Check for replacement
            for (int pid : pids) {
                if (procNameMatches(m_launchedName, pid)) {
                    m_launchedPid = pid;
                    appendLog(QStringLiteral("[Launcher] Game PID changed → %1").arg(pid));
                    return;
                }
            }
            appendLog(QStringLiteral("[Launcher] Game process exited."));
            stopWatch(m_autoRestoreCb->isChecked());
        }
    }
}

void GamingModeTab::stopWatch(bool restore)
{
    if (m_watchTimer) { m_watchTimer->stop(); m_watchTimer = nullptr; }
    m_watchPhase = QStringLiteral("idle");
    m_launchedPid = -1;
    m_killGameBtn->setEnabled(false);
    m_watchStatusLabel->clear();
    if (restore && m_parked) {
        appendLog(QStringLiteral("[Launcher] Auto-restoring: disabling Gaming Mode…"));
        disableGamingMode();
    }
}

void GamingModeTab::killLaunched()
{
    if (m_launchedPid > 0) {
        ::kill(m_launchedPid, SIGTERM);
        appendLog(QStringLiteral("[Launcher] Sent SIGTERM to PID %1").arg(m_launchedPid));
    }
    stopWatch(m_autoRestoreCb->isChecked());
}

void GamingModeTab::appendLog(const QString &msg)
{
    m_log->append(msg);
    emit logMessage(msg);
}
