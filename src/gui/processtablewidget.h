#pragma once
#include "../processinfo.h"
#include "../ruleengine.h"
#include <QTableWidget>
#include <QSet>
#include <functional>

class ProcessTableWidget : public QTableWidget {
    Q_OBJECT
public:
    ProcessTableWidget(RuleEngine *ruleEngine,
                       std::function<void(const QString &)> logCb,
                       QWidget *parent = nullptr);

    void updateSnapshot(const QList<ProcessInfo> &snapshot);
    void updateThrottled(const QSet<int> &throttled);
    void updatePbExempt(const QSet<int> &exempt);
    void setFilter(const QString &text);

signals:
    void ruleAddRequested(Rule rule);
    void affinityManuallyChanged(int pid);
    void pbExemptToggleRequested(int pid, bool exempt);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void showContextMenu(const QPoint &pos);
    void onHeaderClick(int col);
    void showHeaderMenu(const QPoint &pos);

private:
    static constexpr int COL_COUNT = 8;
    static const QStringList COLUMNS;

    RuleEngine *m_ruleEngine;
    std::function<void(const QString &)> m_logCb;

    QList<ProcessInfo> m_snapshot;
    QSet<int>          m_throttled;
    QSet<int>          m_pbExempt;
    int                m_sortCol  = 2;
    bool               m_sortAsc  = false;
    QString            m_filter;

    void refreshDisplay();
    void updateHeaderLabels();

    struct RowProc { int pid; QString name; int nice; QString affinity; QString ionice; };
    RowProc rowProc(int row) const;
    QList<RowProc> selectedProcs() const;

    void doKill(const RowProc &p, bool force);
    void doKillMany(const QList<RowProc> &procs, bool force);
    void doSetAffinity(const RowProc &p);
    void doSetNice(const RowProc &p);
    void doSetIoNice(const RowProc &p);
    void doAddRule(const RowProc &p);
};
