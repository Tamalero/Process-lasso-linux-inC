#pragma once
#include "../processinfo.h"
#include "../ruleengine.h"
#include <QTableWidget>
#include <QSet>
#include <functional>
#include <optional>

// A value the user just set by hand from the Processes tab. Exactly one of the
// optionals is set — whichever attribute the dialog changed.
struct ManualChange {
    int     pid = 0;
    QString name;
    QString field;      // "affinity" | "nice" | "I/O priority" — for the dialog text
    QString display;    // the new value, human-readable
    std::optional<QString> affinity;
    std::optional<int>     nice;
    std::optional<int>     ioniceClass;
    std::optional<int>     ioniceLevel;
};

class ProcessTableWidget : public QTableWidget {
    Q_OBJECT
public:
    ProcessTableWidget(RuleEngine *ruleEngine,
                       std::function<void(const QString &)> logCb,
                       QWidget *parent = nullptr);

    void updateSnapshot(const QList<ProcessInfo> &snapshot);
    void updateThrottled(const QSet<int> &throttled);
    // The two ProBalance exempt lists, as name patterns (case-insensitive
    // "contains", same rule ProBalance itself applies). The widget matches them
    // itself so the row colour, the Status column and the context-menu ticks all
    // agree with each other by construction.
    void setPbExemptPatterns(const QStringList &permanent, const QStringList &session);
    void setFilter(const QString &text);
    // How many processes in the current snapshot a rule would match.
    int countMatching(const Rule &rule) const;


signals:
    void ruleAddRequested(Rule rule);
    // Emitted after any successful manual set. Covers nice and I/O priority as
    // well as affinity — previously only affinity was reported, so a manual
    // nice change got no protection at all and a matching rule reverted it on
    // the next pass, about half a second later.
    void manualChangeApplied(ManualChange change);
    // Separate signals rather than one with a scope flag: the two exemptions have
    // different lifetimes and different owners (config file vs. this run).
    void pbExemptPermanentToggled(QString name, bool exempt);
    void pbExemptSessionToggled(QString name, bool exempt);

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
    QStringList        m_pbPermanent;
    QStringList        m_pbSession;
    int                m_sortCol  = 2;
    bool               m_sortAsc  = false;
    QString            m_filter;

    void refreshDisplay();
    // Re-read one process's live values from /proc and repaint, instead of
    // waiting up to display_refresh_interval_ms for the next snapshot.
    void refreshRow(int pid);
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
