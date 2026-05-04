#include "mainwindow.h"
#include "../config.h"
#include <QAction>
#include <QJsonArray>
#include <QApplication>
#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTime>
#include <QVBoxLayout>

// ---------- helpers ----------

static QIcon makeTrayIcon(double cpuPct)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const int filled = static_cast<int>(22 * cpuPct / 100.0);
    p.fillRect(0, 22 - filled, 22, filled,
               cpuPct > 80 ? Qt::red : cpuPct > 40 ? QColor(QStringLiteral("#f9e2af")) : Qt::green);
    p.setPen(Qt::white);
    p.drawRect(0, 0, 21, 21);
    return QIcon(pm);
}

// ---------- ctor / dtor ----------

MainWindow::MainWindow(QApplication *app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    m_config = Config::load();

    m_proBalance = new ProBalance(
        m_config[QStringLiteral("probalance")].toObject(),
        [this](const QString &msg){ appendLog(msg); });
    m_monitor = new ProcessMonitor(&m_ruleEngine, m_proBalance, m_config, this);
    m_ruleEngine.loadRules(m_config[QStringLiteral("rules")].toArray());

    buildUi();
    buildTray();

    m_companion = new CompanionWidget(this);
    connect(m_companion, &CompanionWidget::showHideRequested, this, [this]{
        toggleWindow();
        m_companion->setMainVisible(isVisible());
    });
    connect(m_companion, &CompanionWidget::maximizeRequested, this, [this]{
        if (!isVisible()) { show(); raise(); activateWindow(); }
        isMaximized() ? showNormal() : showMaximized();
        m_companion->setMainVisible(true);
    });
    connect(m_companion, &CompanionWidget::gamingToggleRequested,
            m_gamingTab, &GamingModeTab::toggleGamingMode);
    connect(m_companion, &CompanionWidget::quitRequested,
            this, &MainWindow::quitApp);
    // Sync tray checkbox when companion is hidden via its own ✕ button.
    // QWidget has no visibilityChanged signal; install an event filter instead.
    m_companion->installEventFilter(this);

    applyTheme();
    startMonitor();

    setWindowTitle(QStringLiteral("Process Lasso Qt"));
    resize(1100, 750);
}

MainWindow::~MainWindow()
{
    m_monitor->stop();
    m_monitor->wait(3000);
    delete m_proBalance;
    saveConfig();
}

// ---------- UI construction ----------

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // CPU widgets row — each graph gets a header label in its own column
    auto *cpuRow = new QHBoxLayout;
    cpuRow->setSpacing(6);
    {
        auto *col = new QVBoxLayout;
        col->setSpacing(2);
        col->setContentsMargins(0, 0, 0, 0);
        auto *lbl = new QLabel(QStringLiteral("CPU History (avg)"), central);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        col->addWidget(lbl);
        m_cpuHistory = new CpuHistoryWidget(central);
        m_cpuHistory->setFixedHeight(80);
        col->addWidget(m_cpuHistory);
        cpuRow->addLayout(col, 3);
    }
    {
        auto *col = new QVBoxLayout;
        col->setSpacing(2);
        col->setContentsMargins(0, 0, 0, 0);
        auto *lbl = new QLabel(QStringLiteral("Per-Core CPU"), central);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        col->addWidget(lbl);
        m_cpuBars = new CpuBarsWidget(central);
        col->addWidget(m_cpuBars);
        cpuRow->addLayout(col, 2);
    }
    root->addLayout(cpuRow);

    // Main tabs
    m_tabs = new QTabWidget(this);

    // Tab 0: Processes
    {
        auto *w = new QWidget;
        auto *vl = new QVBoxLayout(w);
        vl->setContentsMargins(4, 4, 4, 4);
        auto *filterRow = new QHBoxLayout;
        filterRow->addWidget(new QLabel(QStringLiteral("Filter:"), w));
        auto *filterEdit = new QLineEdit(w);
        filterEdit->setPlaceholderText(QStringLiteral("name or PID…"));
        filterRow->addWidget(filterEdit, 1);
        vl->addLayout(filterRow);

        m_procTable = new ProcessTableWidget(&m_ruleEngine,
            [this](const QString &msg){ appendLog(msg); }, w);
        vl->addWidget(m_procTable, 1);

        connect(filterEdit, &QLineEdit::textChanged,
                m_procTable, &ProcessTableWidget::setFilter);
        connect(m_procTable, &ProcessTableWidget::affinityManuallyChanged,
                this, &MainWindow::onAffinityManualChange);
        connect(m_procTable, &ProcessTableWidget::ruleAddRequested,
                this, &MainWindow::onRuleAddFromTable);

        m_tabs->addTab(w, QStringLiteral("Processes"));
    }

    // Tab 1: Rules
    {
        m_rulesEditor = new RulesEditor(&m_ruleEngine, this);
        connect(m_rulesEditor, &RulesEditor::rulesChanged,
                this, &MainWindow::onRulesChanged);
        m_tabs->addTab(m_rulesEditor, QStringLiteral("Rules"));
    }

    // Tab 2: ProBalance
    {
        m_pbTab = new ProBalanceTab(
            m_config[QStringLiteral("probalance")].toObject(), this);
        connect(m_pbTab, &ProBalanceTab::settingsChanged,
                this, &MainWindow::onPbSettingsChanged);
        m_tabs->addTab(m_pbTab, QStringLiteral("ProBalance"));
    }

    // Tab 3: Gaming Mode
    {
        m_gamingTab = new GamingModeTab(m_config, this);
        connect(m_gamingTab, &GamingModeTab::resetRequested,
                this, &MainWindow::onResetRequested);
        connect(m_gamingTab, &GamingModeTab::gamingModeChanged,
                this, &MainWindow::onGamingModeChanged);
        connect(m_gamingTab, &GamingModeTab::logMessage,
                this, &MainWindow::appendLog);
        connect(m_gamingTab, &GamingModeTab::configChanged,
                this, &MainWindow::onSettingsChanged);
        m_tabs->addTab(m_gamingTab, QStringLiteral("Gaming Mode"));
    }

    // Tab 4: Settings
    {
        m_settingsTab = new SettingsTab(m_config, this);
        connect(m_settingsTab, &SettingsTab::settingsChanged,
                this, &MainWindow::onSettingsChanged);
        m_tabs->addTab(m_settingsTab, QStringLiteral("Settings"));
    }

    // Tab 5: Log
    {
        auto *w = new QWidget;
        auto *vl = new QVBoxLayout(w);
        vl->setContentsMargins(4, 4, 4, 4);
        m_logEdit = new QTextEdit(w);
        m_logEdit->setReadOnly(true);
        m_logEdit->document()->setMaximumBlockCount(2000);
        vl->addWidget(m_logEdit, 1);

        auto *botRow = new QHBoxLayout;
        m_logAutoScroll = new QCheckBox(QStringLiteral("Auto-scroll"), w);
        m_logAutoScroll->setChecked(true);
        auto *clearBtn = new QPushButton(QStringLiteral("Clear"), w);
        connect(clearBtn, &QPushButton::clicked, m_logEdit, &QTextEdit::clear);
        botRow->addWidget(m_logAutoScroll);
        botRow->addStretch();
        botRow->addWidget(clearBtn);
        vl->addLayout(botRow);

        m_tabs->addTab(w, QStringLiteral("Log"));
    }

    root->addWidget(m_tabs, 1);
    statusBar()->showMessage(QStringLiteral("Ready"));
}

void MainWindow::buildTray()
{
    m_tray = new QSystemTrayIcon(makeTrayIcon(0), this);
    m_tray->setToolTip(QStringLiteral("Process Lasso Qt"));

    auto *menu = new QMenu(this);
    auto *showAct = menu->addAction(QStringLiteral("Show / Hide"),
        this, &MainWindow::toggleWindow);
    showAct->setFont([]{
        QFont f; f.setBold(true); return f;
    }());
    menu->addAction(QStringLiteral("Maximize / Restore"), this, [this]{
        if (!isVisible()) { show(); raise(); activateWindow(); }
        isMaximized() ? showNormal() : showMaximized();
        if (m_companion) m_companion->setMainVisible(true);
    });
    m_trayGamingAction = menu->addAction(QStringLiteral("Enable Gaming Mode"),
        this, [this]{ m_gamingTab->toggleGamingMode(); });
    menu->addSeparator();
    m_trayCompanionAction = menu->addAction(QStringLiteral("Companion Panel"));
    m_trayCompanionAction->setCheckable(true);
    connect(m_trayCompanionAction, &QAction::toggled, this, [this](bool on){
        if (on) m_companion->show(); else m_companion->hide();
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Quit"), this, &MainWindow::quitApp);
    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated,
        this, [this](QSystemTrayIcon::ActivationReason r){
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
                toggleWindow();
        });

    m_tray->show();
}

// ---------- monitor ----------

void MainWindow::startMonitor()
{
    connect(m_monitor, &ProcessMonitor::processSnapshotReady,
            this, &MainWindow::onSnapshot, Qt::QueuedConnection);
    connect(m_monitor, &ProcessMonitor::cpuSnapshotReady,
            this, [this](const QList<double> &percpu){
                m_cpuBars->updateCpu(percpu);
                m_cpuHistory->updateCpu(percpu);
                onCpuForTray(percpu);
                m_companion->updateCpu(percpu);
            }, Qt::QueuedConnection);
    connect(m_monitor, &ProcessMonitor::logMessage,
            this, &MainWindow::appendLog, Qt::QueuedConnection);
    m_monitor->start();
}

// ---------- config ----------

void MainWindow::saveConfig()
{
    m_config[QStringLiteral("rules")] = m_ruleEngine.toJsonArray();
    Config::save(m_config);
}

// ---------- theme ----------

void MainWindow::applyTheme()
{
    if (m_config[QStringLiteral("system_theme")].toBool(false)) {
        m_app->setStyleSheet({});
        return;
    }
    // Catppuccin Mocha-inspired dark theme
    m_app->setStyleSheet(QStringLiteral(R"(
        QWidget                { background:#1e1e2e; color:#cdd6f4; font-size:13px; }
        QMainWindow            { background:#1e1e2e; }
        QTabWidget::pane       { border:1px solid #313244; }
        QTabBar::tab           { background:#181825; color:#bac2de; padding:6px 14px;
                                  border-top-left-radius:6px; border-top-right-radius:6px; }
        QTabBar::tab:selected  { background:#313244; color:#cdd6f4; }
        QTabBar::tab:hover     { background:#45475a; }
        QGroupBox              { border:1px solid #313244; border-radius:6px;
                                  margin-top:10px; padding-top:6px; }
        QGroupBox::title       { subcontrol-origin:margin; left:8px; color:#89b4fa; }
        QTableWidget           { gridline-color:#313244; selection-background-color:#45475a; }
        QHeaderView::section   { background:#181825; color:#89b4fa; border:1px solid #313244;
                                  padding:3px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background:#313244; border:1px solid #45475a; border-radius:4px;
            padding:3px 6px; color:#cdd6f4; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border:1px solid #89b4fa; }
        QPushButton            { background:#45475a; border:1px solid #585b70;
                                  border-radius:5px; padding:4px 12px; color:#cdd6f4; }
        QPushButton:hover      { background:#585b70; }
        QPushButton:pressed    { background:#313244; }
        QCheckBox::indicator   { width:14px; height:14px; border:1px solid #585b70;
                                  border-radius:3px; background:#313244; }
        QCheckBox::indicator:checked { background:#89b4fa; }
        QTextEdit              { background:#181825; border:1px solid #313244;
                                  border-radius:4px; }
        QScrollBar:vertical    { background:#181825; width:10px; }
        QScrollBar::handle:vertical { background:#45475a; border-radius:5px; min-height:20px; }
        QStatusBar             { background:#181825; color:#6c7086; }
        QMenu                  { background:#1e1e2e; border:1px solid #313244; }
        QMenu::item:selected   { background:#45475a; }
        QSlider::groove:horizontal { height:4px; background:#313244; border-radius:2px; }
        QSlider::handle:horizontal { background:#89b4fa; width:14px; height:14px;
                                      margin:-5px 0; border-radius:7px; }
    )"));
    const int opacity = m_config[QStringLiteral("window_opacity")].toInt(100);
    setWindowOpacity(opacity / 100.0);
}

// ---------- window toggle / quit ----------

void MainWindow::toggleWindow()
{
    if (isVisible() && !isMinimized()) hide();
    else { show(); raise(); activateWindow(); }
    if (m_companion) m_companion->setMainVisible(isVisible());
}

void MainWindow::quitApp()
{
    saveConfig();
    m_monitor->stop();
    m_monitor->wait(3000);
    m_tray->hide();
    m_app->quit();
}

// ---------- close event ----------

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_companion && event->type() == QEvent::Hide)
        if (m_trayCompanionAction) m_trayCompanionAction->setChecked(false);
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_tray && m_tray->isSystemTrayAvailable()) {
        hide();
        event->ignore();
    } else {
        quitApp();
        event->accept();
    }
}

// ---------- slots ----------

void MainWindow::onSnapshot(const QList<ProcessInfo> &snapshot)
{
    m_procTable->updateSnapshot(snapshot);
    m_procTable->updateThrottled(m_proBalance->throttledPids());
    statusBar()->showMessage(
        QStringLiteral("%1 processes").arg(snapshot.size()));
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), msg);
    m_logEdit->append(line);
    if (m_logAutoScroll->isChecked())
        m_logEdit->verticalScrollBar()->setValue(
            m_logEdit->verticalScrollBar()->maximum());
}

void MainWindow::onCpuForTray(const QList<double> &percpu)
{
    if (percpu.isEmpty()) return;
    double sum = 0;
    for (double v : percpu) sum += v;
    const double avg = sum / percpu.size();
    m_tray->setIcon(makeTrayIcon(avg));
    m_tray->setToolTip(
        QStringLiteral("Process Lasso Qt — CPU: %1%").arg(avg, 0, 'f', 1));
}

void MainWindow::onRulesChanged()
{
    saveConfig();
    m_monitor->reapplyAllDefaults();
}

void MainWindow::onAffinityManualChange(int pid)
{
    m_monitor->setManualAffinityOverride(pid, 30.0);
}

void MainWindow::onRuleAddFromTable(Rule rule)
{
    m_rulesEditor->addRuleDirect(rule);
}

void MainWindow::onPbSettingsChanged(QJsonObject pbCfg)
{
    m_config[QStringLiteral("probalance")] = pbCfg;
    m_proBalance->updateConfig(pbCfg);
    saveConfig();
}

void MainWindow::onResetRequested()
{
    m_monitor->resetAllAffinities();
    appendLog(QStringLiteral("Reset all affinities to defaults"));
}

void MainWindow::onGamingModeChanged(bool active, bool elevateNice)
{
    m_monitor->setGamingMode(active, elevateNice);
    updateTrayGamingAction(active, elevateNice);
    m_companion->setGamingActive(active);
    appendLog(active ? QStringLiteral("Gaming mode enabled")
                     : QStringLiteral("Gaming mode disabled"));
}

void MainWindow::onSettingsChanged(QJsonObject updatedConfig)
{
    m_config = Config::deepMerge(m_config, updatedConfig);
    m_monitor->updateConfig(m_config);
    m_settingsTab->updateConfig(m_config);
    applyTheme();
    saveConfig();
}

void MainWindow::updateTrayGamingAction(bool active, bool)
{
    if (m_trayGamingAction)
        m_trayGamingAction->setText(
            active ? QStringLiteral("Disable Gaming Mode")
                   : QStringLiteral("Enable Gaming Mode"));
}
