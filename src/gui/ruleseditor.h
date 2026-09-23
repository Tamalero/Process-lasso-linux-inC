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
    void reapplyRequested();

private:
    RuleEngine   *m_engine;
    QTableWidget *m_table = nullptr;

    QString selectedRuleId() const;
    const Rule *findDuplicate(const Rule &candidate) const;
    bool        confirmDuplicate(const Rule &existing, const Rule &candidate);
    void    addRule();
    void    editSelected();
    void    deleteSelected();
    void    toggleSelected();
    void    showPresets();
    void    exportRules();
    void    importRules();
};
