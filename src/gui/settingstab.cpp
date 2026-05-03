#include "settingstab.h"
#include "dialogs.h"
#include "../cputopology.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

static const QString kServiceDir  = QDir::homePath() + QStringLiteral("/.config/systemd/user");
static const QString kServiceFile = kServiceDir + QStringLiteral("/process-lasso.service");

SettingsTab::SettingsTab(const QJsonObject &cfg, QWidget *parent)
    : QWidget(parent), m_config(cfg)
{
    auto *layout = new QVBoxLayout(this);

    // --- CPU group ---
    auto *cpuGroup = new QGroupBox(QStringLiteral("Default Process Affinity"), this);
    auto *cpuForm  = new QFormLayout(cpuGroup);
    m_defaultAffinityCb = new QCheckBox(QStringLiteral("Apply default affinity to new processes"), this);
    cpuForm->addRow(m_defaultAffinityCb);

    auto *affinRow = new QHBoxLayout;
    m_defaultAffinityEdit = new QLineEdit(this);
    m_defaultAffinityEdit->setPlaceholderText(QStringLiteral("e.g. 0-7 or 0,2,4,6"));
    auto *pickBtn    = new QPushButton(QStringLiteral("Pick…"), this);
    auto *quickAll   = new QPushButton(QStringLiteral("All"), this);
    auto *quickPcore = new QPushButton(QStringLiteral("P-cores"), this);
    connect(pickBtn,    &QPushButton::clicked, this, &SettingsTab::pickAffinity);
    connect(quickAll,   &QPushButton::clicked, this, [this]{ setQuick({}); });
    connect(quickPcore, &QPushButton::clicked, this, [this]{ setQuick(QStringLiteral("pcores")); });
    affinRow->addWidget(m_defaultAffinityEdit, 1);
    affinRow->addWidget(pickBtn);
    affinRow->addWidget(quickAll);
    affinRow->addWidget(quickPcore);
    cpuForm->addRow(QStringLiteral("CPU list:"), affinRow);

    auto *cpuApply = new QPushButton(QStringLiteral("Apply CPU Settings"), this);
    connect(cpuApply, &QPushButton::clicked, this, &SettingsTab::applyCpu);
    cpuForm->addRow(cpuApply);
    layout->addWidget(cpuGroup);

    // --- Monitor intervals ---
    auto *monGroup = new QGroupBox(QStringLiteral("Monitor Intervals"), this);
    auto *monForm  = new QFormLayout(monGroup);
    m_ruleInterval = new QSpinBox(this);
    m_ruleInterval->setRange(100, 10000);
    m_ruleInterval->setSingleStep(100);
    m_ruleInterval->setSuffix(QStringLiteral(" ms"));
    monForm->addRow(QStringLiteral("Rule enforcement interval:"), m_ruleInterval);
    m_displayInterval = new QSpinBox(this);
    m_displayInterval->setRange(500, 10000);
    m_displayInterval->setSingleStep(500);
    m_displayInterval->setSuffix(QStringLiteral(" ms"));
    monForm->addRow(QStringLiteral("Display refresh interval:"), m_displayInterval);
    auto *monApply = new QPushButton(QStringLiteral("Apply Intervals"), this);
    connect(monApply, &QPushButton::clicked, this, &SettingsTab::applyMonitor);
    monForm->addRow(monApply);
    layout->addWidget(monGroup);

    // --- Appearance ---
    auto *appGroup = new QGroupBox(QStringLiteral("Appearance"), this);
    auto *appForm  = new QFormLayout(appGroup);
    m_systemThemeCb = new QCheckBox(QStringLiteral("Follow system theme (dark/light)"), this);
    appForm->addRow(m_systemThemeCb);

    auto *opacRow = new QHBoxLayout;
    m_opacitySlider = new QSlider(Qt::Horizontal, this);
    m_opacitySlider->setRange(30, 100);
    m_opacitySlider->setTickInterval(10);
    m_opacityLabel = new QLabel(QStringLiteral("100%"), this);
    m_opacityLabel->setMinimumWidth(40);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v){
        m_opacityLabel->setText(QString::number(v) + QStringLiteral("%"));
        if (window()) window()->setWindowOpacity(v / 100.0);
    });
    opacRow->addWidget(m_opacitySlider, 1);
    opacRow->addWidget(m_opacityLabel);
    appForm->addRow(QStringLiteral("Window opacity:"), opacRow);

    auto *appApply = new QPushButton(QStringLiteral("Apply Appearance"), this);
    connect(appApply, &QPushButton::clicked, this, [this]{
        QJsonObject cfg = m_config;
        cfg[QStringLiteral("system_theme")]   = m_systemThemeCb->isChecked();
        cfg[QStringLiteral("window_opacity")] = m_opacitySlider->value();
        m_config = cfg;
        emit settingsChanged(cfg);
    });
    appForm->addRow(appApply);
    layout->addWidget(appGroup);

    // --- Autostart ---
    auto *startGroup = new QGroupBox(QStringLiteral("Autostart"), this);
    auto *startForm  = new QFormLayout(startGroup);
    m_autostartCb = new QCheckBox(
        QStringLiteral("Start with desktop session (systemd user service)"), this);
    connect(m_autostartCb, &QCheckBox::toggled, this, &SettingsTab::applyAutostart);
    startForm->addRow(m_autostartCb);
    layout->addWidget(startGroup);

    layout->addStretch();
    loadConfig();
}

void SettingsTab::updateConfig(const QJsonObject &cfg)
{
    m_config = cfg;
    loadConfig();
}

void SettingsTab::loadConfig()
{
    const auto cpu = m_config[QStringLiteral("cpu")].toObject();
    m_defaultAffinityCb->setChecked(
        cpu[QStringLiteral("apply_default_affinity")].toBool(false));
    m_defaultAffinityEdit->setText(
        cpu[QStringLiteral("default_affinity")].toString());

    const auto mon = m_config[QStringLiteral("monitor")].toObject();
    m_ruleInterval->setValue(mon[QStringLiteral("rule_interval_ms")].toInt(500));
    m_displayInterval->setValue(mon[QStringLiteral("display_interval_ms")].toInt(2000));

    m_systemThemeCb->setChecked(m_config[QStringLiteral("system_theme")].toBool(false));
    m_opacitySlider->setValue(m_config[QStringLiteral("window_opacity")].toInt(100));
    m_opacityLabel->setText(QString::number(m_opacitySlider->value()) + QStringLiteral("%"));

    m_autostartCb->blockSignals(true);
    m_autostartCb->setChecked(QFile::exists(kServiceFile));
    m_autostartCb->blockSignals(false);
}

void SettingsTab::pickAffinity()
{
    AffinityDialog dlg(m_defaultAffinityEdit->text(), this, QStringLiteral("Default"));
    if (dlg.exec() == QDialog::Accepted)
        m_defaultAffinityEdit->setText(dlg.getCpulist());
}

void SettingsTab::setQuick(const QString &val)
{
    if (val.isEmpty()) {
        m_defaultAffinityEdit->clear();
    } else if (val == QStringLiteral("pcores")) {
        const CpuTopology topo = detectTopology();
        m_defaultAffinityEdit->setText(formatCpuSet(topo.preferred));
    }
}

void SettingsTab::applyCpu()
{
    QJsonObject cpu = m_config[QStringLiteral("cpu")].toObject();
    cpu[QStringLiteral("apply_default_affinity")] = m_defaultAffinityCb->isChecked();
    cpu[QStringLiteral("default_affinity")]        = m_defaultAffinityEdit->text().trimmed();
    m_config[QStringLiteral("cpu")] = cpu;
    emit settingsChanged(m_config);
}

void SettingsTab::applyMonitor()
{
    QJsonObject mon = m_config[QStringLiteral("monitor")].toObject();
    mon[QStringLiteral("rule_interval_ms")]    = m_ruleInterval->value();
    mon[QStringLiteral("display_interval_ms")] = m_displayInterval->value();
    m_config[QStringLiteral("monitor")] = mon;
    emit settingsChanged(m_config);
}

void SettingsTab::applyAutostart()
{
    const bool enable = m_autostartCb->isChecked();
    if (enable) {
        QDir().mkpath(kServiceDir);
        QFile f(kServiceFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("Autostart"),
                QStringLiteral("Could not write service file:\n%1").arg(f.errorString()));
            m_autostartCb->blockSignals(true);
            m_autostartCb->setChecked(false);
            m_autostartCb->blockSignals(false);
            return;
        }
        const QString exe = QCoreApplication::applicationFilePath();
        f.write(QStringLiteral(
            "[Unit]\n"
            "Description=Process Lasso Qt\n"
            "After=graphical-session.target\n\n"
            "[Service]\n"
            "ExecStart=%1\n"
            "Restart=on-failure\n\n"
            "[Install]\n"
            "WantedBy=default.target\n"
        ).arg(exe).toUtf8());
        f.close();
        QProcess::execute(QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("enable"),
             QStringLiteral("process-lasso.service")});
    } else {
        QProcess::execute(QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("disable"),
             QStringLiteral("process-lasso.service")});
        QFile::remove(kServiceFile);
    }
}
