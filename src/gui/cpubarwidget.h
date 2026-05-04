#pragma once
#include <QWidget>
#include <QList>
#include <QSet>
#include <QHash>

class CpuBarsWidget : public QWidget {
    Q_OBJECT
public:
    explicit CpuBarsWidget(QWidget *parent = nullptr);
public slots:
    void updateCpu(const QList<double> &percpu);
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

    int  cols(int n) const;
    int  barIndexAt(const QPoint &pos) const;
    void applyNeededHeight();
    void readTemps();
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
