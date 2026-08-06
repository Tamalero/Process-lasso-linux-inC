#pragma once
#include <QWidget>
#include <QColor>
#include <QList>
#include <QSet>
#include <QHash>

// Absolute-temperature colour ramp (blue → green → yellow → peach → red).
QColor temperatureColor(double celsius);

class CpuBarsWidget : public QWidget {
    Q_OBJECT
public:
    explicit CpuBarsWidget(QWidget *parent = nullptr);
public slots:
    void updateCpu(const QList<double> &percpu);
    // Per-logical-CPU temperatures, pushed from the monitor thread.
    void setTemps(const QHash<int,double> &temps);
    // Controls the numeric °C readout only; the heat tint on the bar fill is
    // always applied when a temperature is known.
    void setShowTemps(bool show);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *ev) override;
    bool event(QEvent *ev) override;
private:
    QList<double>    m_pcts;
    QSet<int>        m_offline;
    QHash<int,double> m_temps;
    QHash<int,double> m_freqs;
    bool             m_showTemps = true;

    int  cols(int n) const;
    int  barIndexAt(const QPoint &pos) const;
    void applyNeededHeight();
    void readFreqs();
};

class CpuHistoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit CpuHistoryWidget(QWidget *parent = nullptr);
public slots:
    void updateCpu(const QList<double> &percpu);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    static constexpr int HISTORY_LEN = 120;
    QList<double> m_history;
};
