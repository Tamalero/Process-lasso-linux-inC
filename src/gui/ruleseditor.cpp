#include "ruleseditor.h"
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
    layout->addLayout(btnRow);
    refresh();
}

void RulesEditor::refresh()
{
    const auto &rules = m_engine->rules();
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
        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells[col]);
            item->setData(Qt::UserRole, r.ruleId);
            m_table->setItem(row, col, item);
        }
    }
}

void RulesEditor::addRuleDirect(const Rule &rule)
{
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

void RulesEditor::addRule()
{
    RuleEditDialog dlg(nullptr, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_engine->addRule(dlg.getRule());
        refresh(); emit rulesChanged();
    }
}

void RulesEditor::editSelected()
{
    const QString id = selectedRuleId(); if (id.isEmpty()) return;
    const auto &rules = m_engine->rules();
    const auto it = std::find_if(rules.cbegin(), rules.cend(), [&](const Rule &r){ return r.ruleId == id; });
    if (it == rules.cend()) return;
    RuleEditDialog dlg(&(*it), this);
    if (dlg.exec() == QDialog::Accepted) {
        m_engine->updateRule(dlg.getRule());
        refresh(); emit rulesChanged();
    }
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
