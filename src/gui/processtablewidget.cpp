#include "processtablewidget.h"
#include "dialogs.h"
#include "../probalance.h"
#include "../utils.h"
#include <QAction>
#include <QColor>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <algorithm>
#include <csignal>

const QStringList ProcessTableWidget::COLUMNS = {
    QStringLiteral("PID"), QStringLiteral("Name"), QStringLiteral("CPU%"),
    QStringLiteral("Mem(MB)"), QStringLiteral("Nice"), QStringLiteral("Affinity"),
    QStringLiteral("I/O"), QStringLiteral("Status")
};

ProcessTableWidget::ProcessTableWidget(RuleEngine *re,
                                        std::function<void(const QString &)> logCb,
                                        QWidget *parent)
    : QTableWidget(0, COL_COUNT, parent), m_ruleEngine(re), m_logCb(std::move(logCb))
{
    setHorizontalHeaderLabels(COLUMNS);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    verticalHeader()->setVisible(false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTableWidget::customContextMenuRequested, this, &ProcessTableWidget::showContextMenu);
    auto *hdr = horizontalHeader();
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionsClickable(true);
    connect(hdr, &QHeaderView::sectionClicked, this, &ProcessTableWidget::onHeaderClick);
    hdr->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(hdr, &QHeaderView::customContextMenuRequested, this, &ProcessTableWidget::showHeaderMenu);
    setSortingEnabled(false);
    updateHeaderLabels();
}

void ProcessTableWidget::updateHeaderLabels()
{
    QStringList labels;
    for (int i = 0; i < COLUMNS.size(); ++i) {
        if (i == m_sortCol)
            labels << COLUMNS[i] + (m_sortAsc ? QStringLiteral(" ▲") : QStringLiteral(" ▼"));
        else
            labels << COLUMNS[i];
    }
    setHorizontalHeaderLabels(labels);
}

void ProcessTableWidget::onHeaderClick(int col)
{
    if (m_sortCol == col) m_sortAsc = !m_sortAsc;
    else { m_sortCol = col; m_sortAsc = (col != 2 && col != 3); }
    updateHeaderLabels();
    refreshDisplay();
}

void ProcessTableWidget::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    for (int i = 0; i < COLUMNS.size(); ++i) {
        auto *act = menu.addAction(COLUMNS[i]);
        act->setCheckable(true);
        act->setChecked(!horizontalHeader()->isSectionHidden(i));
        act->setData(i);
    }
    if (auto *chosen = menu.exec(horizontalHeader()->mapToGlobal(pos))) {
        const int col = chosen->data().toInt();
        horizontalHeader()->setSectionHidden(col, !horizontalHeader()->isSectionHidden(col));
    }
}

void ProcessTableWidget::updateSnapshot(const QList<ProcessInfo> &snapshot)
{
    m_snapshot = snapshot;
    refreshDisplay();
}

void ProcessTableWidget::updateThrottled(const QSet<int> &throttled)
{
    m_throttled = throttled;
}

void ProcessTableWidget::setPbExemptPatterns(const QStringList &permanent,
                                             const QStringList &session)
{
    m_pbPermanent = permanent;
    m_pbSession   = session;
    refreshDisplay();
}

void ProcessTableWidget::refreshRow(int pid)
{
    // The snapshot the table draws from is up to display_refresh_interval_ms
    // old (2 s by default). Leaving a hand-made change invisible for that long
    // reads as "it was reverted" — and while the GUI thread was starved before
    // v1.4.1, it read as "it never applied at all". Patch the row now.
    for (auto &p : m_snapshot) {
        if (p.pid != pid) continue;
        p.affinity = Utils::getAffinityStr(pid);
        int n = 0;
        if (Utils::getNice(pid, n)) p.nice = n;
        int cls = 0, lvl = 0;
        if (Utils::getIoNice(pid, cls, lvl))
            p.ionice = QStringLiteral("%1/%2").arg(cls).arg(lvl);
        break;
    }
    refreshDisplay();
}

int ProcessTableWidget::countMatching(const Rule &rule) const
{
    int n = 0;
    for (const auto &p : m_snapshot)
        if (rule.matches(p.name)) ++n;
    return n;
}

void ProcessTableWidget::setFilter(const QString &text)
{
    m_filter = text.trimmed().toLower();
    refreshDisplay();
}

void ProcessTableWidget::refreshDisplay()
{
    QList<ProcessInfo> sorted = m_snapshot;
    std::sort(sorted.begin(), sorted.end(), [this](const ProcessInfo &a, const ProcessInfo &b) {
        // Swap lo/hi for descending to keep strict weak ordering (never negate a < result).
        const ProcessInfo &lo = m_sortAsc ? a : b;
        const ProcessInfo &hi = m_sortAsc ? b : a;
        switch (m_sortCol) {
            case 0: return lo.pid        < hi.pid;
            case 1: return lo.name.toLower() < hi.name.toLower();
            case 2: return lo.cpuPercent < hi.cpuPercent;
            case 3: return lo.memRss     < hi.memRss;
            case 4: return lo.nice       < hi.nice;
            case 5: return lo.affinity   < hi.affinity;
            case 6: return lo.ionice     < hi.ionice;
            default: return false;
        }
    });
    if (!m_filter.isEmpty()) {
        sorted.erase(std::remove_if(sorted.begin(), sorted.end(), [this](const ProcessInfo &p){
            return !p.name.toLower().contains(m_filter) && !QString::number(p.pid).contains(m_filter);
        }), sorted.end());
    }
    setRowCount(sorted.size());
    for (int row = 0; row < sorted.size(); ++row) {
        const auto &proc = sorted[row];
        const bool throttled  = m_throttled.contains(proc.pid);
        const bool permExempt = ProBalance::nameMatchesAny(proc.name, m_pbPermanent);
        const bool sessExempt = ProBalance::nameMatchesAny(proc.name, m_pbSession);
        const bool pbExempt   = permExempt || sessExempt;
        QColor rowColor;
        if      (throttled)           rowColor = QColor(QStringLiteral("#fab387"));
        else if (pbExempt)            rowColor = QColor(QStringLiteral("#89dceb"));
        else if (proc.cpuPercent>=80) rowColor = QColor(QStringLiteral("#f38ba8"));
        else if (proc.cpuPercent>=40) rowColor = QColor(QStringLiteral("#f9e2af"));
        else if (proc.cpuPercent>=10) rowColor = QColor(QStringLiteral("#a6e3a1"));
        QString statusText;
        if (throttled) statusText = QStringLiteral("⏸ Throttled");
        else if (permExempt) statusText = QStringLiteral("⚡ PB Exempt");
        else if (sessExempt) statusText = QStringLiteral("⚡ PB Exempt (session)");
        const QStringList items = {
            QString::number(proc.pid), proc.name,
            QStringLiteral("%1").arg(proc.cpuPercent, 0, 'f', 1),
            QStringLiteral("%1").arg(proc.memRss / 1048576.0, 0, 'f', 1),
            QString::number(proc.nice), proc.affinity, proc.ionice,
            statusText
        };
        for (int col = 0; col < COL_COUNT; ++col) {
            auto *item = new QTableWidgetItem(items[col]);
            item->setTextAlignment(Qt::AlignVCenter |
                (col == 2 || col == 3 || col == 4 ? Qt::AlignRight : Qt::AlignLeft));
            if (col == 1 && !proc.cmdline.isEmpty()) item->setToolTip(proc.cmdline);
            if (rowColor.isValid()) item->setForeground(rowColor);
            setItem(row, col, item);
        }
    }
}

ProcessTableWidget::RowProc ProcessTableWidget::rowProc(int row) const
{
    return {
        item(row,0) ? item(row,0)->text().toInt() : 0,
        item(row,1) ? item(row,1)->text() : QString{},
        item(row,4) ? item(row,4)->text().toInt() : 0,
        item(row,5) ? item(row,5)->text() : QString{},
        item(row,6) ? item(row,6)->text() : QString{}
    };
}

QList<ProcessTableWidget::RowProc> ProcessTableWidget::selectedProcs() const
{
    QSet<int> rows;
    for (const auto &idx : selectedIndexes()) rows.insert(idx.row());
    QList<RowProc> result;
    for (int r : rows) result.append(rowProc(r));
    return result;
}

void ProcessTableWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        const auto procs = selectedProcs();
        if (!procs.isEmpty()) { doKillMany(procs, false); return; }
    }
    QTableWidget::keyPressEvent(event);
}

void ProcessTableWidget::showContextMenu(const QPoint &pos)
{
    const auto procs = selectedProcs();
    if (procs.isEmpty()) return;
    const auto proc = rowProc(currentRow());
    QMenu menu(this);
    if (procs.size() > 1) {
        menu.addAction(QStringLiteral("Kill %1 selected").arg(procs.size()),
            [this, procs]{ doKillMany(procs, false); });
        menu.addAction(QStringLiteral("Force Kill %1 selected").arg(procs.size()),
            [this, procs]{ doKillMany(procs, true); });
        menu.addSeparator();
    } else {
        menu.addAction(QStringLiteral("Kill %1 (%2)").arg(proc.name).arg(proc.pid),
            [this, proc]{ doKill(proc, false); });
        menu.addAction(QStringLiteral("Force Kill %1 (%2)").arg(proc.name).arg(proc.pid),
            [this, proc]{ doKill(proc, true); });
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Set Affinity for %1…").arg(proc.name),
        [this, proc]{ doSetAffinity(proc); });
    menu.addAction(QStringLiteral("Set Priority (nice) for %1…").arg(proc.name),
        [this, proc]{ doSetNice(proc); });
    menu.addAction(QStringLiteral("Set I/O Priority for %1…").arg(proc.name),
        [this, proc]{ doSetIoNice(proc); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Add Rule for '%1'…").arg(proc.name),
        [this, proc]{ doAddRule(proc); });
    menu.addSeparator();
    // Name-based, not pid-based: an exemption is a name pattern, so it covers
    // every process of that name. Two independent lifetimes, shown as two
    // checkable items so the current state is visible rather than inferred from
    // which verb the menu happens to be offering.
    auto *pbMenu = menu.addMenu(QStringLiteral("ProBalance exemption for '%1'").arg(proc.name));

    auto *sessionAct = pbMenu->addAction(QStringLiteral("This session only"));
    sessionAct->setCheckable(true);
    const bool sessOn = ProBalance::nameMatchesAny(proc.name, m_pbSession);
    sessionAct->setChecked(sessOn);
    sessionAct->setToolTip(QStringLiteral(
        "Exempt until Process Lasso Qt exits. Not written to config.json."));
    connect(sessionAct, &QAction::triggered, this,
        [this, name = proc.name, sessOn]{ emit pbExemptSessionToggled(name, !sessOn); });

    auto *permAct = pbMenu->addAction(QStringLiteral("Permanent (saved to config)"));
    permAct->setCheckable(true);
    const bool permOn = ProBalance::nameMatchesAny(proc.name, m_pbPermanent);
    permAct->setChecked(permOn);
    permAct->setToolTip(QStringLiteral(
        "Adds the name to the ProBalance tab's exempt list and saves it."));
    connect(permAct, &QAction::triggered, this,
        [this, name = proc.name, permOn]{ emit pbExemptPermanentToggled(name, !permOn); });

    pbMenu->setToolTipsVisible(true);
    menu.exec(viewport()->mapToGlobal(pos));
}

void ProcessTableWidget::doKill(const RowProc &p, bool force)
{
    QString msg;
    if (::kill(p.pid, force ? SIGKILL : SIGTERM) == 0)
        msg = QStringLiteral("%1illed %2 (%3)").arg(force ? QStringLiteral("Force k")
                                                           : QStringLiteral("K")).arg(p.name).arg(p.pid);
    else
        msg = QStringLiteral("Kill failed for %1 (%2)").arg(p.name).arg(p.pid);
    if (m_logCb) m_logCb(msg);
}

void ProcessTableWidget::doKillMany(const QList<RowProc> &procs, bool force)
{
    QStringList names;
    for (int i = 0; i < std::min((int)procs.size(), 4); ++i)
        names << QStringLiteral("%1(%2)").arg(procs[i].name).arg(procs[i].pid);
    if (procs.size() > 4) names << QStringLiteral("and %1 more").arg(procs.size()-4);
    if (QMessageBox::question(this,
            QStringLiteral("Confirm Kill"),
            QStringLiteral("%1 %2 processes?\n%3")
                .arg(force ? QStringLiteral("Force kill") : QStringLiteral("Kill"))
                .arg(procs.size()).arg(names.join(QStringLiteral(", "))),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    for (const auto &p : procs) doKill(p, force);
}

void ProcessTableWidget::doSetAffinity(const RowProc &p)
{
    // Seed from the kernel, not from the table cell: the cell can be seconds
    // stale, so the dialog would open showing a mask the process no longer has.
    const QString live = Utils::getAffinityStr(p.pid);
    AffinityDialog dlg(live.isEmpty() ? p.affinity : live, this, p.name);
    if (dlg.exec() == QDialog::Accepted) {
        const QString cpulist = dlg.getCpulist();
        if (Utils::setAffinity(p.pid, cpulist)) {
            if (m_logCb) m_logCb(QStringLiteral("Set affinity=%1 on %2(%3)").arg(cpulist, p.name).arg(p.pid));
            ManualChange c;
            c.pid = p.pid; c.name = p.name;
            c.field = QStringLiteral("affinity"); c.display = cpulist;
            c.affinity = cpulist;
            refreshRow(p.pid);
            emit manualChangeApplied(c);
        } else {
            if (m_logCb) m_logCb(QStringLiteral("Failed to set affinity on %1(%2)").arg(p.name).arg(p.pid));
        }
    }
}

void ProcessTableWidget::doSetNice(const RowProc &p)
{
    int liveNice = p.nice;
    Utils::getNice(p.pid, liveNice);
    NicePriorityDialog dlg(liveNice, this, p.name);
    if (dlg.exec() == QDialog::Accepted) {
        const int nice = dlg.getNice();
        if (Utils::setNice(p.pid, nice)) {
            if (m_logCb) m_logCb(QStringLiteral("Set nice=%1 on %2(%3)").arg(nice).arg(p.name).arg(p.pid));
            ManualChange c;
            c.pid = p.pid; c.name = p.name;
            c.field = QStringLiteral("nice"); c.display = QString::number(nice);
            c.nice = nice;
            refreshRow(p.pid);
            emit manualChangeApplied(c);
        }
        else
            { if (m_logCb) m_logCb(QStringLiteral("Failed to set nice=%1 on %2(%3) (root needed?)").arg(nice).arg(p.name).arg(p.pid)); }
    }
}

void ProcessTableWidget::doSetIoNice(const RowProc &p)
{
    // Was hardcoded to class 2 / level 4, so the dialog never showed what the
    // process was actually set to and silently proposed changing it.
    int curCls = 2, curLvl = 4;
    Utils::getIoNice(p.pid, curCls, curLvl);
    IoNiceDialog dlg(curCls, curLvl, this, p.name);
    if (dlg.exec() == QDialog::Accepted) {
        const int cls = dlg.getIoNiceClass(), lvl = dlg.getIoNiceLevel();
        if (Utils::setIoNice(p.pid, cls, lvl)) {
            if (m_logCb) m_logCb(QStringLiteral("Set ionice class=%1 level=%2 on %3(%4)").arg(cls).arg(lvl).arg(p.name).arg(p.pid));
            ManualChange c;
            c.pid = p.pid; c.name = p.name;
            c.field = QStringLiteral("I/O priority");
            c.display = QStringLiteral("class %1 level %2").arg(cls).arg(lvl);
            c.ioniceClass = cls; c.ioniceLevel = lvl;
            refreshRow(p.pid);
            emit manualChangeApplied(c);
        }
        else
            { if (m_logCb) m_logCb(QStringLiteral("Failed to set ionice on %1(%2)").arg(p.name).arg(p.pid)); }
    }
}

void ProcessTableWidget::doAddRule(const RowProc &p)
{
    Rule templ;
    templ.name = p.name; templ.pattern = p.name; templ.matchType = QStringLiteral("contains");
    RuleEditDialog dlg(&templ, this);
    if (dlg.exec() == QDialog::Accepted) emit ruleAddRequested(dlg.getRule());
}
