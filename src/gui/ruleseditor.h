#pragma once
#include "../ruleengine.h"
#include <QWidget>
#include <QTableWidget>

class RulesEditor : public QWidget {
    Q_OBJECT
public:
    explicit RulesEditor(RuleEngine *engine, QWidget *parent = nullptr);
    void addRuleDirect(const Rule &rule);
    void refresh();

signals:
    void rulesChanged();

private:
    RuleEngine   *m_engine;
    QTableWidget *m_table = nullptr;

    QString selectedRuleId() const;
    void    addRule();
    void    editSelected();
    void    deleteSelected();
    void    toggleSelected();
    void    showPresets();
    void    exportRules();
    void    importRules();
};
