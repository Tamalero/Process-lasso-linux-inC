#include "ruleseditor.h"
#include <QColor>
#include <QFont>
#include "dialogs.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

RulesEditor::RulesEditor(RuleEngine *engine, QWidget *parent)
    : QWidget(parent), m_engine(engine)
{
    auto *layout = new QVBoxLayout(this);
    const QStringList cols = {
        QStringLiteral("Enabled"), QStringLiteral("Name"), QStringLiteral("Pattern"),
        QStringLiteral("Match"), QStringLiteral("Affinity"), QStringLiteral("Nice"),
        QStringLiteral("I/O Class"), QStringLiteral("I/O Lvl")
    };
    m_table = new QTableWidget(0, cols.size(), this);
    m_table->setHorizontalHeaderLabels(cols);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::doubleClicked, this, &RulesEditor::editSelected);
    layout->addWidget(m_table);

    auto *btnRow = new QHBoxLayout;
    auto makeBtn = [&](const QString &label, auto slot){
        auto *btn = new QPushButton(label, this);
        connect(btn, &QPushButton::clicked, this, slot);
        btnRow->addWidget(btn);
    };
    makeBtn(QStringLiteral("Add Rule"),    &RulesEditor::addRule);
    makeBtn(QStringLiteral("Templates…"), &RulesEditor::showPresets);
    makeBtn(QStringLiteral("Edit"),        &RulesEditor::editSelected);
    makeBtn(QStringLiteral("Delete"),      &RulesEditor::deleteSelected);
    makeBtn(QStringLiteral("Enable/Disable"), &RulesEditor::toggleSelected);
    makeBtn(QStringLiteral("Export…"),     &RulesEditor::exportRules);
    makeBtn(QStringLiteral("Import…"),     &RulesEditor::importRules);
    btnRow->addStretch();
    {
        auto *reapply = new QPushButton(QStringLiteral("Refresh && Reapply Rules"), this);
        reapply->setToolTip(QStringLiteral(
            "Redraws the tables from the live state, forces a rule pass "
            "immediately, and reports the result in the Log.\n\n"
            "Rules are already enforced automatically about twice a second, so "
            "this does not make them apply more often — it tells you what they "
            "actually did, including anything that could not be applied.\n\n"
            "If pressing this visibly changes what the tables show, something "
            "was stale that should not have been: please report it."));
        connect(reapply, &QPushButton::clicked, this, [this]{ emit reapplyRequested(); });
        btnRow->addWidget(reapply);
    }
    layout->addLayout(btnRow);
    refresh();
}

void RulesEditor::refresh()
{
    const auto &rules = m_engine->rules();
    const auto shadow = m_engine->shadowedAttributes();
    m_table->setRowCount(rules.size());
    for (int row = 0; row < rules.size(); ++row) {
        const auto &r = rules[row];
        const QStringList cells = {
            r.enabled ? QStringLiteral("Yes") : QStringLiteral("No"),
            r.name, r.pattern, r.matchType,
            r.affinity.value_or(QString{}),
            r.nice ? QString::number(*r.nice) : QString{},
            r.ioniceClass ? QString::number(*r.ioniceClass) : QString{},
            r.ioniceLevel ? QString::number(*r.ioniceLevel) : QString{}
        };
        const QStringList shadowed = shadow.value(r.ruleId);
        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells[col]);
            item->setData(Qt::UserRole, r.ruleId);
            // Grey out and strike through any attribute an earlier rule with the
            // same pattern already claims: it is in the config but can never take
            // effect, and leaving it looking live is how you end up believing a
            // rule is broken.
            const bool dead =
                (col == 4 && shadowed.contains(QStringLiteral("affinity")))
             || (col == 5 && shadowed.contains(QStringLiteral("nice")))
             || ((col == 6 || col == 7) && shadowed.contains(QStringLiteral("I/O priority")));
            if (dead && !cells[col].isEmpty()) {
                QFont f = item->font();
                f.setStrikeOut(true);
                item->setFont(f);
                item->setForeground(QColor(QStringLiteral("#6c7086")));
                item->setToolTip(QStringLiteral(
                    "Never applied: an earlier rule with the same pattern (\"%1\", %2) "
                    "already sets this. The first matching rule wins, per attribute.")
                        .arg(r.pattern, r.matchType));
            }
            m_table->setItem(row, col, item);
        }
    }
}

void RulesEditor::addRuleDirect(const Rule &rule)
{
    // Reached from the Processes tab's "Add Rule for '<name>'…", which is exactly
    // how a second rule for an already-covered process gets created by accident.
    if (const Rule *dup = findDuplicate(rule)) {
        if (!confirmDuplicate(*dup, rule)) return;
    }
    m_engine->addRule(rule);
    refresh();
    emit rulesChanged();
}

QString RulesEditor::selectedRuleId() const
{
    const int row = m_table->currentRow();
    if (row < 0) return {};
    const auto *item = m_table->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString{};
}

// Returns the first enabled rule that targets exactly the same processes as
// `candidate` (same pattern, same match type), ignoring `candidate` itself.
const Rule *RulesEditor::findDuplicate(const Rule &candidate) const
{
    for (const auto &r : m_engine->rules()) {
        if (r.ruleId == candidate.ruleId) continue;
        if (!r.enabled) continue;
        if (r.matchType == candidate.matchType
         && r.pattern.compare(candidate.pattern, Qt::CaseInsensitive) == 0)
            return &r;
    }
    return nullptr;
}

// True if the user still wants to go ahead after being told about `existing`.
bool RulesEditor::confirmDuplicate(const Rule &existing, const Rule &candidate)
{
    QStringList clashes;
    if (existing.affinity    && candidate.affinity)    clashes << QStringLiteral("affinity");
    if (existing.nice        && candidate.nice)        clashes << QStringLiteral("priority");
    if (existing.ioniceClass && candidate.ioniceClass) clashes << QStringLiteral("I/O priority");

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("A rule for this already exists"));
    box.setText(QStringLiteral("<b>%1</b> already matches <b>%2</b> (%3).")
                    .arg(existing.name, candidate.pattern, candidate.matchType));
    box.setInformativeText(clashes.isEmpty()
        ? QStringLiteral("Two rules matching the same processes is allowed, but the "
                         "first one wins for any setting they both define.")
        : QStringLiteral("Both rules set <b>%1</b>. Only the first one takes effect — "
                         "the other's %1 is ignored entirely and will be shown struck "
                         "through.<br><br>Edit the existing rule instead?")
              .arg(clashes.join(QStringLiteral(" and "))));
    auto *editBtn = box.addButton(QStringLiteral("Edit existing rule"), QMessageBox::AcceptRole);
    auto *addBtn  = box.addButton(QStringLiteral("Add anyway"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(editBtn);
    box.exec();

    if (box.clickedButton() == addBtn) return true;
    if (box.clickedButton() == editBtn) {
        const Rule target = existing;          // copy: refresh() invalidates the ref
        RuleEditDialog edit(&target, this);
        if (edit.exec() == QDialog::Accepted) {
            m_engine->updateRule(edit.getRule());
            refresh(); emit rulesChanged();
        }
    }
    return false;
}

void RulesEditor::addRule()
{
    RuleEditDialog dlg(nullptr, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const Rule candidate = dlg.getRule();
    if (const Rule *dup = findDuplicate(candidate)) {
        if (!confirmDuplicate(*dup, candidate)) return;
    }
    m_engine->addRule(candidate);
    refresh(); emit rulesChanged();
}

void RulesEditor::editSelected()
{
    const QString id = selectedRuleId(); if (id.isEmpty()) return;
    const auto &rules = m_engine->rules();
    const auto it = std::find_if(rules.cbegin(), rules.cend(), [&](const Rule &r){ return r.ruleId == id; });
    if (it == rules.cend()) return;
    RuleEditDialog dlg(&(*it), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const Rule edited = dlg.getRule();
    // Editing a rule's pattern can turn it into a duplicate. Warn — but NEVER
    // throw the edit away, and never offer "edit the existing rule" here: the
    // user is already editing a specific rule, and silently discarding their
    // change (or redirecting them to a different rule) is how "editing rules
    // does nothing" happens. Only an explicit Cancel abandons it.
    if (const Rule *dup = findDuplicate(edited)) {
        QStringList clashes;
        if (dup->affinity    && edited.affinity)    clashes << QStringLiteral("affinity");
        if (dup->nice        && edited.nice)        clashes << QStringLiteral("priority");
        if (dup->ioniceClass && edited.ioniceClass) clashes << QStringLiteral("I/O priority");
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Another rule matches the same processes"));
        box.setText(QStringLiteral("<b>%1</b> also matches <b>%2</b> (%3).")
                        .arg(dup->name, edited.pattern, edited.matchType));
        box.setInformativeText(clashes.isEmpty()
            ? QStringLiteral("Save this change anyway?")
            : QStringLiteral("Both set <b>%1</b>. Only the first matching rule takes "
                             "effect for those; the other is shown struck through."
                             "<br><br>Save this change anyway?").arg(clashes.join(QStringLiteral(" and "))));
        auto *saveBtn = box.addButton(QStringLiteral("Save change"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Discard change"), QMessageBox::RejectRole);
        box.setDefaultButton(saveBtn);
        box.exec();
        if (box.clickedButton() != saveBtn) return;
    }
    m_engine->updateRule(edited);
    refresh(); emit rulesChanged();
}

void RulesEditor::deleteSelected()
{
    const QString id = selectedRuleId(); if (id.isEmpty()) return;
    const auto &rules = m_engine->rules();
    const auto it = std::find_if(rules.cbegin(), rules.cend(), [&](const Rule &r){ return r.ruleId == id; });
    if (it == rules.cend()) return;
    if (QMessageBox::question(this, QStringLiteral("Delete Rule"),
            QStringLiteral("Delete rule '%1'?").arg(it->name),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    m_engine->removeRule(id);
    refresh(); emit rulesChanged();
}

void RulesEditor::toggleSelected()
{
    const QString id = selectedRuleId(); if (id.isEmpty()) return;
    auto &rules = const_cast<QList<Rule> &>(m_engine->rules()); // via updateRule
    const auto it = std::find_if(rules.cbegin(), rules.cend(), [&](const Rule &r){ return r.ruleId == id; });
    if (it == rules.cend()) return;
    Rule toggled = *it; toggled.enabled = !toggled.enabled;
    m_engine->updateRule(toggled);
    refresh(); emit rulesChanged();
}

void RulesEditor::showPresets()
{
    RulePresetsDialog pdlg(this);
    if (pdlg.exec() != QDialog::Accepted) return;
    const auto *preset = pdlg.getPreset();
    if (!preset) return;
    Rule templ;
    templ.name = preset->name; templ.pattern = preset->pattern;
    templ.matchType = preset->matchType; templ.affinity = preset->affinity.isEmpty() ? std::nullopt : std::optional<QString>(preset->affinity);
    templ.nice = preset->nice; templ.ioniceClass = preset->ioniceClass; templ.ioniceLevel = preset->ioniceLevel;
    RuleEditDialog dlg(&templ, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_engine->addRule(dlg.getRule());
        refresh(); emit rulesChanged();
    }
}

void RulesEditor::exportRules()
{
    const QString path = QFileDialog::getSaveFileName(this,
        QStringLiteral("Export Rules"), QStringLiteral("process_lasso_rules.json"),
        QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Export Failed"), f.errorString()); return;
    }
    f.write(QJsonDocument(m_engine->toJsonArray()).toJson(QJsonDocument::Indented));
    QMessageBox::information(this, QStringLiteral("Export"),
        QStringLiteral("Exported %1 rules to %2").arg(m_engine->rules().size()).arg(path));
}

void RulesEditor::importRules()
{
    const QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("Import Rules"), {}, QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import Failed"), f.errorString()); return;
    }
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        QMessageBox::warning(this, QStringLiteral("Import Failed"),
            QStringLiteral("Invalid JSON: %1").arg(err.errorString())); return;
    }
    int imported = 0;
    for (const auto &v : doc.array()) {
        if (v.isObject()) { m_engine->addRule(Rule::fromJson(v.toObject())); ++imported; }
    }
    refresh(); emit rulesChanged();
    QMessageBox::information(this, QStringLiteral("Import"),
        QStringLiteral("Imported %1 rules from %2").arg(imported).arg(path));
}
