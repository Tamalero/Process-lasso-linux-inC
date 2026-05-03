#include "probalancetab.h"
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

ProBalanceTab::ProBalanceTab(const QJsonObject &cfg, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    m_enabledCb = new QCheckBox(QStringLiteral("ProBalance Enabled"), this);
    m_enabledCb->setChecked(cfg[QStringLiteral("enabled")].toBool(true));
    layout->addWidget(m_enabledCb);

    auto *throttleGroup = new QGroupBox(QStringLiteral("Throttle Settings"), this);
    auto *form = new QFormLayout(throttleGroup);
    m_cpuThresh = new QDoubleSpinBox(this);
    m_cpuThresh->setRange(10.0, 100.0); m_cpuThresh->setSingleStep(5.0); m_cpuThresh->setSuffix(QStringLiteral(" %"));
    m_cpuThresh->setValue(cfg[QStringLiteral("cpu_threshold_percent")].toDouble(85.0));
    form->addRow(QStringLiteral("CPU threshold:"), m_cpuThresh);
    m_consecSecs = new QSpinBox(this);
    m_consecSecs->setRange(1, 60); m_consecSecs->setSuffix(QStringLiteral(" s"));
    m_consecSecs->setValue(cfg[QStringLiteral("consecutive_seconds")].toInt(3));
    form->addRow(QStringLiteral("Consecutive seconds above threshold:"), m_consecSecs);
    m_niceAdj = new QSpinBox(this);
    m_niceAdj->setRange(1, 19); m_niceAdj->setValue(cfg[QStringLiteral("nice_adjustment")].toInt(10));
    form->addRow(QStringLiteral("Nice adjustment (added on throttle):"), m_niceAdj);
    m_niceFloor = new QSpinBox(this);
    m_niceFloor->setRange(1, 19); m_niceFloor->setValue(cfg[QStringLiteral("nice_floor")].toInt(15));
    form->addRow(QStringLiteral("Nice floor (max nice applied):"), m_niceFloor);
    layout->addWidget(throttleGroup);

    auto *restoreGroup = new QGroupBox(QStringLiteral("Restore Settings"), this);
    auto *form2 = new QFormLayout(restoreGroup);
    m_restoreThresh = new QDoubleSpinBox(this);
    m_restoreThresh->setRange(1.0, 99.0); m_restoreThresh->setSingleStep(5.0); m_restoreThresh->setSuffix(QStringLiteral(" %"));
    m_restoreThresh->setValue(cfg[QStringLiteral("restore_threshold_percent")].toDouble(40.0));
    form2->addRow(QStringLiteral("Restore when CPU below:"), m_restoreThresh);
    m_restoreHyst = new QSpinBox(this);
    m_restoreHyst->setRange(1, 120); m_restoreHyst->setSuffix(QStringLiteral(" s"));
    m_restoreHyst->setValue(cfg[QStringLiteral("restore_hysteresis_seconds")].toInt(5));
    form2->addRow(QStringLiteral("Restore hysteresis (seconds below restore threshold):"), m_restoreHyst);
    layout->addWidget(restoreGroup);

    auto *exemptGroup = new QGroupBox(QStringLiteral("Exempt Processes (pattern contains)"), this);
    auto *exLayout = new QVBoxLayout(exemptGroup);
    m_exemptList = new QListWidget(this);
    for (const auto &v : cfg[QStringLiteral("exempt_patterns")].toArray())
        m_exemptList->addItem(v.toString());
    exLayout->addWidget(m_exemptList);
    auto *exEditRow = new QHBoxLayout;
    m_exemptEdit = new QLineEdit(this);
    m_exemptEdit->setPlaceholderText(QStringLiteral("Pattern to exempt..."));
    auto *addBtn = new QPushButton(QStringLiteral("Add"), this);
    auto *delBtn = new QPushButton(QStringLiteral("Remove selected"), this);
    connect(addBtn, &QPushButton::clicked, this, &ProBalanceTab::addExempt);
    connect(delBtn, &QPushButton::clicked, this, &ProBalanceTab::delExempt);
    exEditRow->addWidget(m_exemptEdit); exEditRow->addWidget(addBtn); exEditRow->addWidget(delBtn);
    exLayout->addLayout(exEditRow);
    layout->addWidget(exemptGroup);

    auto *applyBtn = new QPushButton(QStringLiteral("Apply Settings"), this);
    connect(applyBtn, &QPushButton::clicked, this, &ProBalanceTab::apply);
    layout->addWidget(applyBtn);
    layout->addStretch();
}

void ProBalanceTab::addExempt()
{
    const QString text = m_exemptEdit->text().trimmed();
    if (!text.isEmpty()) { m_exemptList->addItem(text); m_exemptEdit->clear(); }
}

void ProBalanceTab::delExempt()
{
    for (auto *item : m_exemptList->selectedItems())
        delete m_exemptList->takeItem(m_exemptList->row(item));
}

void ProBalanceTab::apply()
{
    emit settingsChanged(getConfig());
    QMessageBox::information(this, QStringLiteral("ProBalance"), QStringLiteral("Settings applied."));
}

QJsonObject ProBalanceTab::getConfig() const
{
    QJsonArray exempt;
    for (int i = 0; i < m_exemptList->count(); ++i)
        exempt.append(m_exemptList->item(i)->text());
    QJsonObject cfg;
    cfg[QStringLiteral("enabled")]                   = m_enabledCb->isChecked();
    cfg[QStringLiteral("cpu_threshold_percent")]      = m_cpuThresh->value();
    cfg[QStringLiteral("consecutive_seconds")]        = m_consecSecs->value();
    cfg[QStringLiteral("nice_adjustment")]            = m_niceAdj->value();
    cfg[QStringLiteral("nice_floor")]                 = m_niceFloor->value();
    cfg[QStringLiteral("restore_threshold_percent")]  = m_restoreThresh->value();
    cfg[QStringLiteral("restore_hysteresis_seconds")] = m_restoreHyst->value();
    cfg[QStringLiteral("exempt_patterns")]            = exempt;
    return cfg;
}
