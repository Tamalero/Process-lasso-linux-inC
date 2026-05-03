#include "dialogs.h"
#include "../cputopology.h"
#include "../utils.h"
#include <QApplication>
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>
#include <sys/syscall.h>
#include <unistd.h>

// ── AffinityDialog ────────────────────────────────────────────────────────────

AffinityDialog::AffinityDialog(const QString &currentAffinity,
                                QWidget *parent,
                                const QString &titleSuffix)
    : QDialog(parent)
{
    setWindowTitle(titleSuffix.isEmpty()
        ? QStringLiteral("Set CPU Affinity")
        : QStringLiteral("Set CPU Affinity — %1").arg(titleSuffix));
    m_cpuCount = Utils::getCpuCount();
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    const auto offline = getOfflineCpuSet();
    const auto topo    = detectTopology();
    const auto smtSibs = getSmtSiblingsOf(topo.preferred | topo.nonPreferred);

    // Parse current selection
    QSet<int> selected;
    if (currentAffinity.isEmpty()) {
        for (int i = 0; i < m_cpuCount; ++i) selected.insert(i);
    } else {
        selected = Utils::cpulistToSet(currentAffinity);
    }

    auto *group = new QGroupBox(QStringLiteral("Select CPUs"), this);
    auto *grid  = new QGridLayout(group);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);

    QHash<int, QCheckBox *> cbMap;

    if (topo.hasAsymmetry()) {
        const bool isX3D = (topo.kind == TopologyKind::AmdX3D);
        const QString ccd0Name = isX3D
            ? QStringLiteral("CCD0 (V-Cache — preferred)")
            : QStringLiteral("Preferred CCD");
        const QString ccd1Name = isX3D
            ? QStringLiteral("CCD1 (parked in Gaming Mode)")
            : QStringLiteral("Non-preferred CCD (parked in Gaming Mode)");

        int row = 0;
        auto addSection = [&](const QString &title, const QList<int> &cpus) {
            auto *hdr = new QLabel(title, this);
            hdr->setStyleSheet(QStringLiteral(
                "font-size: 11px; font-weight: 600; "
                "color: rgba(167,139,250,0.9); padding-top: 6px;"));
            grid->addWidget(hdr, row++, 0, 1, 8);
            for (int col = 0; col < cpus.size(); ++col) {
                int cpu = cpus[col];
                auto *cb = new QCheckBox(QString::number(cpu));
                cb->setChecked(selected.contains(cpu));
                if (offline.contains(cpu)) {
                    cb->setEnabled(false);
                    cb->setToolTip(QStringLiteral("CPU %1 is parked").arg(cpu));
                }
                grid->addWidget(cb, row, col % 8);
                cbMap[cpu] = cb;
                if (col % 8 == 7 && col < cpus.size()-1) ++row;
            }
            ++row;
        };

        auto prefPhys  = (topo.preferred  - smtSibs).values(); std::sort(prefPhys.begin(),  prefPhys.end());
        auto prefHt    = (topo.preferred  & smtSibs).values(); std::sort(prefHt.begin(),    prefHt.end());
        auto nprefPhys = (topo.nonPreferred- smtSibs).values();std::sort(nprefPhys.begin(), nprefPhys.end());
        auto nprefHt   = (topo.nonPreferred& smtSibs).values();std::sort(nprefHt.begin(),   nprefHt.end());

        if (!prefPhys.isEmpty())  addSection(ccd0Name + QStringLiteral(" — physical cores"), prefPhys);
        if (!prefHt.isEmpty())    addSection(ccd0Name + QStringLiteral(" — HT siblings"),    prefHt);
        if (!nprefPhys.isEmpty()) addSection(ccd1Name + QStringLiteral(" — physical cores"), nprefPhys);
        if (!nprefHt.isEmpty())   addSection(ccd1Name + QStringLiteral(" — HT siblings"),    nprefHt);
    } else {
        for (int i = 0; i < m_cpuCount; ++i) {
            auto *cb = new QCheckBox(QString::number(i));
            cb->setChecked(selected.contains(i));
            if (offline.contains(i)) {
                cb->setEnabled(false);
                cb->setToolTip(QStringLiteral("CPU %1 is parked").arg(i));
            }
            grid->addWidget(cb, i/8, i%8);
            cbMap[i] = cb;
        }
    }

    m_checkboxes.resize(m_cpuCount);
    for (int i = 0; i < m_cpuCount; ++i)
        m_checkboxes[i] = cbMap.value(i, new QCheckBox(QString::number(i)));

    if (!offline.isEmpty()) {
        auto *note = new QLabel(
            QStringLiteral("⚠  CPUs %1 are parked (Gaming Mode active).")
                .arg(Utils::cpusetToCpulist(offline)), this);
        note->setStyleSheet(QStringLiteral("color: rgba(249,226,175,0.85); font-size: 11px;"));
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    layout->addWidget(group);

    auto *btnRow = new QHBoxLayout;
    auto *allBtn  = new QPushButton(QStringLiteral("All"), this);
    auto *noneBtn = new QPushButton(QStringLiteral("None"), this);
    connect(allBtn,  &QPushButton::clicked, this, &AffinityDialog::selectAll);
    connect(noneBtn, &QPushButton::clicked, this, &AffinityDialog::selectNone);
    btnRow->addWidget(allBtn);
    btnRow->addWidget(noneBtn);
    if (topo.hasAsymmetry()) {
        const bool isX3D = (topo.kind == TopologyKind::AmdX3D);
        auto *ccd0Btn = new QPushButton(isX3D ? QStringLiteral("CCD0 (V-Cache)")
                                               : QStringLiteral("Preferred CCD"), this);
        auto *ccd1Btn = new QPushButton(isX3D ? QStringLiteral("CCD1")
                                               : QStringLiteral("Non-preferred CCD"), this);
        connect(ccd0Btn, &QPushButton::clicked, this, [this, pref = topo.preferred]{ selectSet(pref); });
        connect(ccd1Btn, &QPushButton::clicked, this, [this, np = topo.nonPreferred]{ selectSet(np); });
        btnRow->addWidget(ccd0Btn);
        btnRow->addWidget(ccd1Btn);
    }
    btnRow->addStretch();
    layout->addLayout(btnRow);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AffinityDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    adjustSize();
}

void AffinityDialog::selectAll()
{
    for (auto *cb : m_checkboxes) if (cb->isEnabled()) cb->setChecked(true);
}
void AffinityDialog::selectNone()
{
    for (auto *cb : m_checkboxes) if (cb->isEnabled()) cb->setChecked(false);
}
void AffinityDialog::selectSet(const QSet<int> &cpus)
{
    for (int i = 0; i < m_checkboxes.size(); ++i)
        if (m_checkboxes[i]->isEnabled()) m_checkboxes[i]->setChecked(cpus.contains(i));
}
void AffinityDialog::validateAndAccept()
{
    for (auto *cb : m_checkboxes) if (cb->isChecked()) { accept(); return; }
    QMessageBox::warning(this, QStringLiteral("Invalid"),
                         QStringLiteral("At least one CPU must be selected."));
}
QString AffinityDialog::getCpulist() const
{
    QSet<int> sel;
    for (int i = 0; i < m_checkboxes.size(); ++i)
        if (m_checkboxes[i]->isChecked()) sel.insert(i);
    return Utils::cpusetToCpulist(sel);
}

// ── NicePriorityDialog ────────────────────────────────────────────────────────

NicePriorityDialog::NicePriorityDialog(int currentNice, QWidget *parent,
                                        const QString &titleSuffix)
    : QDialog(parent)
{
    setWindowTitle(titleSuffix.isEmpty()
        ? QStringLiteral("Set CPU Priority (nice)")
        : QStringLiteral("Set CPU Priority (nice) — %1").arg(titleSuffix));
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(QStringLiteral(
        "Nice priority: lower = higher priority.\n"
        "Negative values require root."), this);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Nice value:"), this));
    m_spin = new QSpinBox(this);
    m_spin->setRange(-20, 19);
    m_spin->setValue(currentNice);
    row->addWidget(m_spin);
    row->addStretch();
    layout->addLayout(row);
    auto *presets = new QHBoxLayout;
    presets->addWidget(new QLabel(QStringLiteral("Presets:"), this));
    for (auto [label, val] : QList<QPair<QString,int>>{
            {QStringLiteral("High (-10)"), -10},
            {QStringLiteral("Normal (0)"), 0},
            {QStringLiteral("Low (5)"), 5},
            {QStringLiteral("Very Low (15)"), 15},
            {QStringLiteral("Idle (19)"), 19}}) {
        auto *btn = new QPushButton(label, this);
        btn->setFixedWidth(110);
        connect(btn, &QPushButton::clicked, this, [this, v = val]{ m_spin->setValue(v); });
        presets->addWidget(btn);
    }
    presets->addStretch();
    layout->addLayout(presets);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

// ── IoNiceDialog ──────────────────────────────────────────────────────────────

const QList<QPair<int,QString>> IoNiceDialog::CLASSES = {
    {0, QStringLiteral("None (default)")},
    {1, QStringLiteral("Realtime (root)")},
    {2, QStringLiteral("Best-effort")},
    {3, QStringLiteral("Idle")},
};

IoNiceDialog::IoNiceDialog(int currentClass, int currentLevel,
                            QWidget *parent, const QString &titleSuffix)
    : QDialog(parent)
{
    setWindowTitle(titleSuffix.isEmpty()
        ? QStringLiteral("Set I/O Priority")
        : QStringLiteral("Set I/O Priority — %1").arg(titleSuffix));
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(QStringLiteral(
        "I/O class: Realtime requires root. Level 0=highest, 7=lowest."), this);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(QStringLiteral("I/O Class:"), this), 0, 0);
    m_classCombo = new QComboBox(this);
    for (const auto &[val, label] : CLASSES) m_classCombo->addItem(label, val);
    int idx = 0;
    for (int i = 0; i < CLASSES.size(); ++i) if (CLASSES[i].first == currentClass) { idx = i; break; }
    m_classCombo->setCurrentIndex(idx);
    grid->addWidget(m_classCombo, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("I/O Level (0-7):"), this), 1, 0);
    m_levelSpin = new QSpinBox(this);
    m_levelSpin->setRange(0, 7);
    m_levelSpin->setValue(currentLevel);
    grid->addWidget(m_levelSpin, 1, 1);
    layout->addLayout(grid);
    connect(m_classCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]{
        const int cls = m_classCombo->currentData().toInt();
        m_levelSpin->setEnabled(cls == 1 || cls == 2);
    });
    m_levelSpin->setEnabled(currentClass == 1 || currentClass == 2);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
int IoNiceDialog::getIoNiceClass() const { return m_classCombo->currentData().toInt(); }

// ── ProcessPickerDialog ───────────────────────────────────────────────────────

ProcessPickerDialog::ProcessPickerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Select Running Process"));
    setMinimumSize(560, 420);
    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(QStringLiteral("Filter:"), this));
    auto *search = new QLineEdit(this);
    search->setPlaceholderText(QStringLiteral("Type to filter by name…"));
    connect(search, &QLineEdit::textChanged, this, &ProcessPickerDialog::filter);
    searchRow->addWidget(search);
    layout->addLayout(searchRow);
    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("PID"), QStringLiteral("Name"),
                                         QStringLiteral("CPU%"), QStringLiteral("Affinity")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::doubleClicked, this, &ProcessPickerDialog::onAccept);
    layout->addWidget(m_table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProcessPickerDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer::singleShot(0, this, &ProcessPickerDialog::populate);
}

void ProcessPickerDialog::populate()
{
    QList<std::tuple<int,QString,double,QString>> rows;
    const QDir procDir(QStringLiteral("/proc"));
    for (const auto &entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok; int pid = entry.toInt(&ok); if (!ok) continue;
        QFile cf(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!cf.open(QIODevice::ReadOnly)) continue;
        const QString comm = QString(cf.readAll()).trimmed();
        QFile clf(QStringLiteral("/proc/%1/cmdline").arg(pid));
        QStringList cmdline;
        if (clf.open(QIODevice::ReadOnly)) {
            for (const auto &p : clf.readAll().split('\0'))
                if (!p.isEmpty()) cmdline << QString::fromLocal8Bit(p);
        }
        const QString name = Utils::resolveName(comm, cmdline);
        const QString aff  = Utils::getAffinityStr(pid);
        rows.append({pid, name, 0.0, aff});
    }
    std::sort(rows.begin(), rows.end(),
        [](const auto &a, const auto &b){ return std::get<1>(a) < std::get<1>(b); });
    m_allRows = rows;
    renderRows(rows);
}

void ProcessPickerDialog::renderRows(const QList<std::tuple<int,QString,double,QString>> &rows)
{
    m_table->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const auto &[pid, name, cpu, aff] = rows[r];
        m_table->setItem(r, 0, new QTableWidgetItem(QString::number(pid)));
        m_table->setItem(r, 1, new QTableWidgetItem(name));
        auto *cpuItem = new QTableWidgetItem(QStringLiteral("%1").arg(cpu, 0, 'f', 1));
        cpuItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(r, 2, cpuItem);
        m_table->setItem(r, 3, new QTableWidgetItem(aff));
    }
}

void ProcessPickerDialog::filter(const QString &text)
{
    const QString lower = text.toLower();
    QList<std::tuple<int,QString,double,QString>> filtered;
    for (const auto &row : m_allRows)
        if (std::get<1>(row).toLower().contains(lower)) filtered.append(row);
    renderRows(filtered);
}

void ProcessPickerDialog::onAccept()
{
    const int row = m_table->currentRow(); if (row < 0) return;
    m_selectedName     = m_table->item(row,1) ? m_table->item(row,1)->text() : QString{};
    m_selectedAffinity = m_table->item(row,3) ? m_table->item(row,3)->text() : QString{};
    accept();
}

// ── RuleEditDialog ────────────────────────────────────────────────────────────

RuleEditDialog::RuleEditDialog(const Rule *existingRule, QWidget *parent)
    : QDialog(parent), m_rule(existingRule)
{
    setWindowTitle(existingRule ? QStringLiteral("Edit Rule") : QStringLiteral("Add Rule"));
    setMinimumWidth(460);
    auto *layout = new QVBoxLayout(this);

    // Process picker
    auto *pickRow = new QHBoxLayout;
    auto *pickBtn = new QPushButton(QStringLiteral("Select from running processes…"), this);
    connect(pickBtn, &QPushButton::clicked, this, &RuleEditDialog::pickProcess);
    pickRow->addWidget(pickBtn); pickRow->addStretch();
    layout->addLayout(pickRow);

    auto *form = new QFormLayout;
    m_nameEdit    = new QLineEdit(this);
    m_patternEdit = new QLineEdit(this);
    m_matchCombo  = new QComboBox(this);
    for (const auto &s : {QStringLiteral("contains"), QStringLiteral("exact"), QStringLiteral("regex")})
        m_matchCombo->addItem(s);

    m_affinityCb       = new QCheckBox(QStringLiteral("Enable"), this);
    m_affinityDisplay  = new QLineEdit(this);
    m_affinityDisplay->setReadOnly(true);
    m_affinityDisplay->setEnabled(false);
    m_affinityDisplay->setPlaceholderText(QStringLiteral("no affinity set"));
    m_affinityDisplay->setMaximumWidth(160);
    m_affinityPickBtn  = new QPushButton(QStringLiteral("Pick CPUs…"), this);
    m_affinityPickBtn->setEnabled(false);
    connect(m_affinityCb,      &QCheckBox::toggled, m_affinityDisplay, &QLineEdit::setEnabled);
    connect(m_affinityCb,      &QCheckBox::toggled, m_affinityPickBtn, &QPushButton::setEnabled);
    connect(m_affinityPickBtn, &QPushButton::clicked, this, &RuleEditDialog::pickAffinity);

    m_niceCb   = new QCheckBox(QStringLiteral("Enable"), this);
    m_niceSpin = new QSpinBox(this);
    m_niceSpin->setRange(-20, 19); m_niceSpin->setEnabled(false);
    connect(m_niceCb, &QCheckBox::toggled, m_niceSpin, &QSpinBox::setEnabled);

    m_ioniceCb         = new QCheckBox(QStringLiteral("Enable"), this);
    m_ioniceClassCombo = new QComboBox(this);
    for (const auto &[val, label] : IoNiceDialog::CLASSES) m_ioniceClassCombo->addItem(label, val);
    m_ioniceClassCombo->setEnabled(false);
    m_ioniceLevelSpin  = new QSpinBox(this);
    m_ioniceLevelSpin->setRange(0,7); m_ioniceLevelSpin->setEnabled(false);
    connect(m_ioniceCb, &QCheckBox::toggled, m_ioniceClassCombo, &QComboBox::setEnabled);
    connect(m_ioniceCb, &QCheckBox::toggled, m_ioniceLevelSpin,  &QSpinBox::setEnabled);

    m_enabledCb = new QCheckBox(QStringLiteral("Rule enabled"), this);
    m_enabledCb->setChecked(true);

    auto *affRow = new QHBoxLayout;
    affRow->addWidget(m_affinityCb); affRow->addWidget(m_affinityDisplay);
    affRow->addWidget(m_affinityPickBtn); affRow->addStretch();

    auto *niceRow = new QHBoxLayout;
    niceRow->addWidget(m_niceCb); niceRow->addWidget(m_niceSpin); niceRow->addStretch();

    auto *ioniceRow = new QHBoxLayout;
    ioniceRow->addWidget(m_ioniceCb); ioniceRow->addWidget(m_ioniceClassCombo);
    ioniceRow->addWidget(new QLabel(QStringLiteral("Lvl:"), this));
    ioniceRow->addWidget(m_ioniceLevelSpin); ioniceRow->addStretch();

    form->addRow(QStringLiteral("Name:"),        m_nameEdit);
    form->addRow(QStringLiteral("Pattern:"),     m_patternEdit);
    form->addRow(QStringLiteral("Match type:"),  m_matchCombo);
    form->addRow(QStringLiteral("CPU Affinity:"),affRow);
    form->addRow(QStringLiteral("Nice priority:"),niceRow);
    form->addRow(QStringLiteral("I/O priority:"), ioniceRow);
    form->addRow(QStringLiteral(""),              m_enabledCb);
    layout->addLayout(form);

    // Populate if editing
    if (existingRule) {
        m_nameEdit->setText(existingRule->name);
        m_patternEdit->setText(existingRule->pattern);
        const int idx = m_matchCombo->findText(existingRule->matchType);
        if (idx >= 0) m_matchCombo->setCurrentIndex(idx);
        if (existingRule->affinity) { m_affinityCb->setChecked(true); m_affinityDisplay->setText(*existingRule->affinity); }
        if (existingRule->nice)      { m_niceCb->setChecked(true);    m_niceSpin->setValue(*existingRule->nice); }
        if (existingRule->ioniceClass) {
            m_ioniceCb->setChecked(true);
            for (int i = 0; i < IoNiceDialog::CLASSES.size(); ++i)
                if (IoNiceDialog::CLASSES[i].first == *existingRule->ioniceClass) { m_ioniceClassCombo->setCurrentIndex(i); break; }
            if (existingRule->ioniceLevel) m_ioniceLevelSpin->setValue(*existingRule->ioniceLevel);
        }
        m_enabledCb->setChecked(existingRule->enabled);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &RuleEditDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void RuleEditDialog::pickAffinity()
{
    AffinityDialog dlg(m_affinityDisplay->text().trimmed(), this);
    if (dlg.exec() == QDialog::Accepted) m_affinityDisplay->setText(dlg.getCpulist());
}

void RuleEditDialog::pickProcess()
{
    ProcessPickerDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.getSelectedName().isEmpty()) {
        if (m_nameEdit->text().isEmpty()) m_nameEdit->setText(dlg.getSelectedName());
        m_patternEdit->setText(dlg.getSelectedName());
        const int idx = m_matchCombo->findText(QStringLiteral("exact"));
        if (idx >= 0) m_matchCombo->setCurrentIndex(idx);
        if (!m_affinityCb->isChecked() && !dlg.getSelectedAffinity().isEmpty()) {
            m_affinityCb->setChecked(true);
            m_affinityDisplay->setText(dlg.getSelectedAffinity());
        }
    }
}

void RuleEditDialog::validateAndAccept()
{
    if (m_patternEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Validation"), QStringLiteral("Pattern cannot be empty."));
        return;
    }
    if (m_matchCombo->currentText() == QLatin1String("regex")) {
        QRegularExpression re(m_patternEdit->text().trimmed());
        if (!re.isValid()) {
            QMessageBox::warning(this, QStringLiteral("Validation"),
                QStringLiteral("Invalid regular expression:\n%1").arg(re.errorString()));
            return;
        }
    }
    accept();
}

Rule RuleEditDialog::getRule() const
{
    Rule r;
    if (m_rule) r.ruleId = m_rule->ruleId;
    r.name      = m_nameEdit->text().trimmed();
    r.pattern   = m_patternEdit->text().trimmed();
    r.matchType = m_matchCombo->currentText();
    r.enabled   = m_enabledCb->isChecked();
    if (m_affinityCb->isChecked() && !m_affinityDisplay->text().trimmed().isEmpty())
        r.affinity = m_affinityDisplay->text().trimmed();
    if (m_niceCb->isChecked())   r.nice        = m_niceSpin->value();
    if (m_ioniceCb->isChecked()) {
        r.ioniceClass = m_ioniceClassCombo->currentData().toInt();
        r.ioniceLevel = m_ioniceLevelSpin->value();
    }
    return r;
}

// ── SteamGamePickerDialog ─────────────────────────────────────────────────────

SteamGamePickerDialog::SteamGamePickerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Pick Steam Game"));
    setMinimumSize(560, 480);
    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(QStringLiteral("Filter:"), this));
    auto *search = new QLineEdit(this);
    search->setPlaceholderText(QStringLiteral("Type to filter games…"));
    connect(search, &QLineEdit::textChanged, this, &SteamGamePickerDialog::filter);
    searchRow->addWidget(search);
    layout->addLayout(searchRow);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("AppID"), QStringLiteral("Game Name")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::doubleClicked, this, &SteamGamePickerDialog::onAccept);
    layout->addWidget(m_table);
    m_status = new QLabel(QStringLiteral("Scanning Steam library…"), this);
    m_status->setStyleSheet(QStringLiteral("color: #aaa; font-size: 11px;"));
    layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SteamGamePickerDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer::singleShot(0, this, &SteamGamePickerDialog::scanLibrary);
}

void SteamGamePickerDialog::scanLibrary()
{
    QHash<QString, QString> games;
    QSet<QString> seen;
    QStringList libraryDirs;
    for (const auto &root : {QDir::homePath() + QStringLiteral("/.steam/steam"),
                              QDir::homePath() + QStringLiteral("/.local/share/Steam")}) {
        const QString apps = root + QStringLiteral("/steamapps");
        const QString resolved = QFileInfo(apps).canonicalFilePath();
        if (!resolved.isEmpty() && !seen.contains(resolved) && QDir(resolved).exists()) {
            seen.insert(resolved);
            libraryDirs << resolved;
            // Parse libraryfolders.vdf for extra paths
            QFile vdf(resolved + QStringLiteral("/libraryfolders.vdf"));
            if (vdf.open(QIODevice::ReadOnly)) {
                const QString text = QString(vdf.readAll());
                QRegularExpression re(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
                auto it = re.globalMatch(text);
                while (it.hasNext()) {
                    const QString extra = it.next().captured(1) + QStringLiteral("/steamapps");
                    const QString eres = QFileInfo(extra).canonicalFilePath();
                    if (!eres.isEmpty() && !seen.contains(eres) && QDir(eres).exists()) {
                        seen.insert(eres); libraryDirs << eres;
                    }
                }
            }
        }
    }
    for (const auto &dir : libraryDirs) {
        for (const auto &fname : QDir(dir).entryList(QStringList{QStringLiteral("appmanifest_*.acf")}, QDir::Files)) {
            QFile f(dir + '/' + fname);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString text = QString(f.readAll());
            const auto mId   = QRegularExpression(QStringLiteral("\"appid\"\\s+\"(\\d+)\"")).match(text);
            const auto mName = QRegularExpression(QStringLiteral("\"name\"\\s+\"([^\"]+)\"")).match(text);
            if (mId.hasMatch() && mName.hasMatch())
                games[mId.captured(1)] = mName.captured(1);
        }
    }
    m_allRows.clear();
    for (auto it = games.cbegin(); it != games.cend(); ++it)
        m_allRows.append({it.key(), it.value()});
    std::sort(m_allRows.begin(), m_allRows.end(),
        [](const auto &a, const auto &b){ return a.second.toLower() < b.second.toLower(); });
    renderRows(m_allRows);
    m_status->setText(QStringLiteral("%1 games found").arg(m_allRows.size()));
}

void SteamGamePickerDialog::renderRows(const QList<QPair<QString,QString>> &rows)
{
    m_table->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        m_table->setItem(r, 0, new QTableWidgetItem(rows[r].first));
        m_table->setItem(r, 1, new QTableWidgetItem(rows[r].second));
    }
}

void SteamGamePickerDialog::filter(const QString &text)
{
    const QString lower = text.toLower();
    QList<QPair<QString,QString>> filtered;
    for (const auto &[id, name] : m_allRows)
        if (name.toLower().contains(lower) || id.contains(lower)) filtered.append({id, name});
    renderRows(filtered);
}

void SteamGamePickerDialog::onAccept()
{
    const int row = m_table->currentRow(); if (row < 0) return;
    m_appId = m_table->item(row,0) ? m_table->item(row,0)->text() : QString{};
    m_name  = m_table->item(row,1) ? m_table->item(row,1)->text() : QString{};
    accept();
}

// ── LutrisGamePickerDialog ────────────────────────────────────────────────────

LutrisGamePickerDialog::LutrisGamePickerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Pick Lutris Game"));
    setMinimumSize(560, 480);
    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(QStringLiteral("Filter:"), this));
    auto *search = new QLineEdit(this);
    search->setPlaceholderText(QStringLiteral("Type to filter games…"));
    connect(search, &QLineEdit::textChanged, this, &LutrisGamePickerDialog::filter);
    searchRow->addWidget(search);
    layout->addLayout(searchRow);
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Runner / Slug")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::doubleClicked, this, &LutrisGamePickerDialog::onAccept);
    layout->addWidget(m_table);
    m_status = new QLabel(QStringLiteral("Scanning Lutris database…"), this);
    m_status->setStyleSheet(QStringLiteral("color: #aaa; font-size: 11px;"));
    layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &LutrisGamePickerDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer::singleShot(0, this, &LutrisGamePickerDialog::scanLibrary);
}

void LutrisGamePickerDialog::scanLibrary()
{
    // Lutris uses SQLite; query via sqlite3 CLI to avoid a hard dependency
    const QString dbPath = QDir::homePath() + QStringLiteral("/.local/share/lutris/pga.db");
    if (!QFile::exists(dbPath)) {
        m_status->setText(QStringLiteral("Lutris database not found."));
        return;
    }
    QProcess proc;
    proc.start(QStringLiteral("sqlite3"), {dbPath,
        QStringLiteral("SELECT name,slug,runner FROM games WHERE installed=1 ORDER BY name;")});
    if (!proc.waitForFinished(5000)) { m_status->setText(QStringLiteral("sqlite3 timed out.")); return; }
    m_allRows.clear();
    for (const auto &line : proc.readAllStandardOutput().split('\n')) {
        const auto parts = QString(line).trimmed().split('|');
        if (parts.size() >= 2) {
            const QString name   = parts[0];
            const QString slug   = parts[1];
            const QString runner = parts.size() >= 3 ? parts[2] : QString{};
            m_allRows.append({slug, QStringLiteral("%1  [%2]").arg(name, runner)});
        }
    }
    renderRows(m_allRows);
    m_status->setText(QStringLiteral("%1 installed games found").arg(m_allRows.size()));
}

void LutrisGamePickerDialog::renderRows(const QList<QPair<QString,QString>> &rows)
{
    m_table->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const QString name = rows[r].second.split(QStringLiteral("  [")).first();
        const QString runner = rows[r].second.contains(QStringLiteral("  ["))
            ? rows[r].second.section(QStringLiteral("  ["), 1).chopped(1)
            : rows[r].first;
        m_table->setItem(r, 0, new QTableWidgetItem(name));
        m_table->setItem(r, 1, new QTableWidgetItem(runner));
        m_table->item(r, 0)->setData(Qt::UserRole, rows[r].first); // slug
    }
}

void LutrisGamePickerDialog::filter(const QString &text)
{
    const QString lower = text.toLower();
    QList<QPair<QString,QString>> filtered;
    for (const auto &row : m_allRows)
        if (row.second.toLower().contains(lower)) filtered.append(row);
    renderRows(filtered);
}

void LutrisGamePickerDialog::onAccept()
{
    const int row = m_table->currentRow(); if (row < 0) return;
    m_name = m_table->item(row,0) ? m_table->item(row,0)->text() : QString{};
    m_slug = m_table->item(row,0)
        ? m_table->item(row,0)->data(Qt::UserRole).toString()
        : m_name.toLower().replace(' ', '-');
    accept();
}

// ── RulePresetsDialog ─────────────────────────────────────────────────────────

const QList<RulePreset> RulePresetsDialog::PRESETS = {
    {QStringLiteral("Steam (CCD0)"),         QStringLiteral("steam"),         QStringLiteral("exact"),    QStringLiteral("0-7,16-23"),  std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("steamwebhelper"),        QStringLiteral("steamwebhelper"),QStringLiteral("exact"),    QStringLiteral("0-7,16-23"),  5,              std::nullopt, std::nullopt},
    {QStringLiteral("Wine / Proton"),         QStringLiteral("wine"),          QStringLiteral("contains"), QStringLiteral("0-7,16-23"),  std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("OBS Studio"),            QStringLiteral("obs"),           QStringLiteral("exact"),    QStringLiteral("0-7,16-23"),  -1,             std::nullopt, std::nullopt},
    {QStringLiteral("Discord"),               QStringLiteral("discord"),       QStringLiteral("contains"), QStringLiteral("8-15,24-31"), 5,              std::nullopt, std::nullopt},
    {QStringLiteral("Firefox"),               QStringLiteral("firefox"),       QStringLiteral("contains"), QStringLiteral("8-15,24-31"), std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("Chromium / Chrome"),     QStringLiteral("chrom"),         QStringLiteral("contains"), QStringLiteral("8-15,24-31"), std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("Brave"),                 QStringLiteral("brave"),         QStringLiteral("contains"), QStringLiteral("8-15,24-31"), std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("KWin"),                  QStringLiteral("kwin"),          QStringLiteral("contains"), {},                           std::nullopt,   std::nullopt, std::nullopt},
    {QStringLiteral("Plasma Shell"),          QStringLiteral("plasmashell"),   QStringLiteral("exact"),    QStringLiteral("8-15,24-31"), 5,              std::nullopt, std::nullopt},
    {QStringLiteral("Compiler (gcc/clang)"),  QStringLiteral("gcc"),           QStringLiteral("contains"), {},                           std::nullopt,   2,            4},
    {QStringLiteral("Archive / compress"),    QStringLiteral("7z"),            QStringLiteral("contains"), QStringLiteral("8-15,24-31"), 10,             3,            std::nullopt},
    {QStringLiteral("Background (nice 10)"),  {},                              QStringLiteral("contains"), {},                           10,             std::nullopt, std::nullopt},
};

RulePresetsDialog::RulePresetsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Rule Templates"));
    setMinimumSize(680, 400);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Select a preset to create a pre-filled rule:"), this));
    m_table = new QTableWidget((int)PRESETS.size(), 6, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Pattern"),
        QStringLiteral("Match"), QStringLiteral("Affinity"), QStringLiteral("Nice"), QStringLiteral("I/O")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::doubleClicked, this, &QDialog::accept);
    for (int r = 0; r < PRESETS.size(); ++r) {
        const auto &p = PRESETS[r];
        m_table->setItem(r, 0, new QTableWidgetItem(p.name));
        m_table->setItem(r, 1, new QTableWidgetItem(p.pattern));
        m_table->setItem(r, 2, new QTableWidgetItem(p.matchType));
        m_table->setItem(r, 3, new QTableWidgetItem(p.affinity));
        m_table->setItem(r, 4, new QTableWidgetItem(p.nice ? QString::number(*p.nice) : QString{}));
        m_table->setItem(r, 5, new QTableWidgetItem(p.ioniceClass ? QString::number(*p.ioniceClass) : QString{}));
    }
    layout->addWidget(m_table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

const RulePreset *RulePresetsDialog::getPreset() const
{
    const int row = m_table->currentRow();
    return (row >= 0 && row < PRESETS.size()) ? &PRESETS[row] : nullptr;
}
