#include "cpubarwidget.h"
#include "../cputopology.h"
#include "../verbose.h"
#include <QDir>
#include <QFile>
#include <QEvent>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QToolTip>
#include <algorithm>

static const QColor BG(27, 30, 32, 210);
static const QColor BORDER(59, 64, 69, 180);
static const QColor ONLINE_TEXT(239, 240, 241);
static const QColor OFFLINE_TEXT(100, 110, 120, 150);

struct RampEntry { double pct; int r, g, b; };
static const RampEntry RAMP[] = {
    {  0,  34, 197,  94},
    { 25, 163, 230,  53},
    { 50, 234, 179,   8},
    { 70, 249, 115,  22},
    { 85, 239,  68,  68},
    {100, 220,  38,  38},
};

static QColor barColor(double pct)
{
    pct = std::clamp(pct, 0.0, 100.0);
    for (int i = 0; i < 5; ++i) {
        if (pct <= RAMP[i+1].pct) {
            double t = (pct - RAMP[i].pct) / (RAMP[i+1].pct - RAMP[i].pct);
            return QColor(
                (int)(RAMP[i].r + t*(RAMP[i+1].r - RAMP[i].r)),
                (int)(RAMP[i].g + t*(RAMP[i+1].g - RAMP[i].g)),
                (int)(RAMP[i].b + t*(RAMP[i+1].b - RAMP[i].b)));
        }
    }
    return QColor(RAMP[5].r, RAMP[5].g, RAMP[5].b);
}

// Catppuccin-ish ramp for absolute temperatures, shared with the status bar.
QColor temperatureColor(double celsius)
{
    struct Stop { double c; int r, g, b; };
    static const Stop STOPS[] = {
        { 40, 137, 180, 250},   // blue   — idle
        { 60, 166, 227, 161},   // green  — comfortable
        { 75, 249, 226, 175},   // yellow — warm
        { 88, 250, 179, 135},   // peach  — hot
        {100, 243, 139, 168},   // red    — near throttle
    };
    constexpr int N = sizeof(STOPS) / sizeof(STOPS[0]);
    if (celsius <= STOPS[0].c) return QColor(STOPS[0].r, STOPS[0].g, STOPS[0].b);
    for (int i = 0; i < N - 1; ++i) {
        if (celsius <= STOPS[i+1].c) {
            const double t = (celsius - STOPS[i].c) / (STOPS[i+1].c - STOPS[i].c);
            return QColor(
                (int)(STOPS[i].r + t*(STOPS[i+1].r - STOPS[i].r)),
                (int)(STOPS[i].g + t*(STOPS[i+1].g - STOPS[i].g)),
                (int)(STOPS[i].b + t*(STOPS[i+1].b - STOPS[i].b)));
        }
    }
    return QColor(STOPS[N-1].r, STOPS[N-1].g, STOPS[N-1].b);
}

// ── CpuBarsWidget ─────────────────────────────────────────────────────────────

CpuBarsWidget::CpuBarsWidget(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumHeight(30);
    setMouseTracking(true);
}

void CpuBarsWidget::setTemps(const QHash<int,double> &temps)
{
    m_temps = temps;
    update();
}

void CpuBarsWidget::setShowTemps(bool show)
{
    if (m_showTemps == show) return;
    m_showTemps = show;
    if (!show) m_temps.clear();   // monitor stops sending; don't paint stale values
    update();
}

void CpuBarsWidget::readFreqs()
{
    m_freqs.clear();
    for (int cpu = 0; cpu < m_pcts.size(); ++cpu) {
        QFile f(QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq").arg(cpu));
        if (!f.open(QIODevice::ReadOnly)) continue;
        bool ok; double khz = f.readAll().trimmed().toDouble(&ok);
        if (ok) m_freqs[cpu] = khz / 1e6; // GHz
    }
}

void CpuBarsWidget::updateCpu(const QList<double> &percpu)
{
    VLOG("CpuBarsWidget::updateCpu: %lld CPUs, widget %dx%d visible=%d",
         (long long)percpu.size(), width(), height(), (int)isVisible());
    m_pcts = percpu;
    m_offline = getOfflineCpuSet();
    readFreqs();
    applyNeededHeight();
    update();
}

void CpuBarsWidget::applyNeededHeight()
{
    const int n = m_pcts.size();
    if (n == 0) return;
    const int barH = 26, gap = 3;
    const int c = cols(n);
    const int rows = (n + c - 1) / c;
    const int needed = rows * (barH + gap) + gap + 4;
    VLOG("CpuBarsWidget::applyNeededHeight: n=%d cols=%d rows=%d needed=%d current=%d",
         n, c, rows, needed, minimumHeight());
    if (needed != minimumHeight()) {
        setMinimumHeight(needed);
        setMaximumHeight(needed);
        updateGeometry();
    }
}

QSize CpuBarsWidget::sizeHint() const
{
    return QSize(200, minimumHeight());
}

void CpuBarsWidget::resizeEvent(QResizeEvent *ev)
{
    VLOG("CpuBarsWidget::resizeEvent: %dx%d", ev->size().width(), ev->size().height());
    QWidget::resizeEvent(ev);
    applyNeededHeight();
}

int CpuBarsWidget::cols(int n) const
{
    const int w = width() > 0 ? width() : 900;
    const int maxCols = std::min(std::max(1, w / 120), n);
    const int lo = std::max(1, maxCols / 2);
    for (int c = maxCols; c >= lo; --c)
        if (n % c == 0) return c;
    int best = maxCols, bestWaste = (maxCols - n % maxCols) % maxCols;
    for (int c = maxCols-1; c >= lo; --c) {
        int waste = (c - n % c) % c;
        if (waste < bestWaste) { bestWaste = waste; best = c; }
    }
    return best;
}

int CpuBarsWidget::barIndexAt(const QPoint &pos) const
{
    const int n = m_pcts.size(); if (!n) return -1;
    const int barH = 26, gap = 3;
    const int c = cols(n);
    const int barW = std::max(110, (width() - gap*(c+1)) / c);
    for (int i = 0; i < n; ++i) {
        int col = i % c, row = i / c;
        int x = gap + col*(barW+gap), y = gap + row*(barH+gap);
        if (x <= pos.x() && pos.x() <= x+barW && y <= pos.y() && pos.y() <= y+barH)
            return i;
    }
    return -1;
}

bool CpuBarsWidget::event(QEvent *ev)
{
    if (ev->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(ev);
        const int idx = barIndexAt(he->pos());
        if (idx >= 0 && idx < m_pcts.size()) {
            const bool offline = m_offline.contains(idx);
            QString tip = offline
                ? QStringLiteral("CPU %1: offline (parked)").arg(idx)
                : QStringLiteral("CPU %1: %2%").arg(idx).arg(m_pcts[idx], 0, 'f', 1);
            if (!offline) {
                if (m_freqs.contains(idx))
                    tip += QStringLiteral("  |  %1 GHz").arg(m_freqs[idx], 0, 'f', 2);
                if (m_temps.contains(idx))
                    tip += QStringLiteral("  |  %1°C").arg((int)m_temps[idx]);
            }
            QToolTip::showText(he->globalPos(), tip, this);
        } else { QToolTip::hideText(); }
        return true;
    }
    return QWidget::event(ev);
}

void CpuBarsWidget::paintEvent(QPaintEvent *)
{
    const int n = m_pcts.size(); if (!n) return;
    if (gVerbose) {
        static int s_paintCount = 0;
        if (++s_paintCount == 1 || s_paintCount % 20 == 0)
            VLOG("CpuBarsWidget::paintEvent #%d: %d CPUs, widget %dx%d",
                 s_paintCount, n, width(), height());
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int w = width(), barH = 26, gap = 3;
    const int c = cols(n);
    const int barW = std::max(110, (w - gap*(c+1)) / c);
    const int labelW = 52;
    QFont font; font.setPixelSize(10); font.setFamily(QStringLiteral("monospace"));
    QFont freqFont; freqFont.setPixelSize(9); freqFont.setFamily(QStringLiteral("monospace"));
    p.setFont(font);
    for (int i = 0; i < n; ++i) {
        const int col = i%c, row = i/c;
        const int x = gap + col*(barW+gap), y = gap + row*(barH+gap);
        const bool offline = m_offline.contains(i);
        const double pct = m_pcts[i];
        // Background
        p.setPen(Qt::NoPen); p.setBrush(BG);
        p.drawRoundedRect(x, y, barW, barH, 4, 4);
        // Fill
        if (!offline && pct > 0) {
            const int fillW = std::max(0, (int)((barW - labelW - 2) * pct / 100.0));
            QColor col = barColor(pct);
            if (m_temps.contains(i) && m_temps[i] > 40.0) {
                double t = std::min(1.0, (m_temps[i]-40.0)/40.0);
                int a = (int)(t * 120);
                QColor tint(249, 115, 22, a);
                double ta = a / 255.0;
                col = QColor(
                    (int)(col.red()  *(1-ta) + 249*ta),
                    (int)(col.green()*(1-ta) + 115*ta),
                    (int)(col.blue() *(1-ta) +  22*ta));
            }
            p.setBrush(col);
            p.drawRoundedRect(x+labelW+1, y+2, fillW, barH-4, 2, 2);
        }
        // Border
        p.setBrush(Qt::NoBrush); p.setPen(QPen(BORDER, 1));
        p.drawRoundedRect(x, y, barW, barH, 4, 4);
        // CPU label — shifts up to make room for a °C line when one is shown
        const bool tempLine = m_showTemps && !offline && m_temps.contains(i);
        p.setPen(offline ? OFFLINE_TEXT : ONLINE_TEXT);
        p.setFont(font);
        p.drawText(x+2, y, labelW-2, tempLine ? barH-10 : barH,
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Core %1").arg(i));
        if (tempLine) {
            p.setFont(freqFont);
            p.setPen(temperatureColor(m_temps[i]));
            p.drawText(x+2, y+barH-11, labelW-2, 11,
                       Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("%1°C").arg(qRound(m_temps[i])));
            p.setFont(font);
        }
        // Percentage
        if (offline) {
            p.setPen(OFFLINE_TEXT);
            p.drawText(x+labelW+2, y, barW-labelW-4, barH-10,
                       Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("off"));
        } else {
            p.setPen(pct >= 50 ? QColor(255,255,255,220) : ONLINE_TEXT);
            p.drawText(x+labelW+2, y, barW-labelW-4, barH-10,
                       Qt::AlignVCenter | Qt::AlignRight,
                       QStringLiteral("%1%").arg((int)pct));
            if (m_freqs.contains(i)) {
                p.setFont(freqFont);
                p.setPen(QColor(180, 200, 220, 160));
                p.drawText(x+labelW+2, y+barH-11, barW-labelW-4, 11,
                           Qt::AlignVCenter | Qt::AlignRight,
                           QStringLiteral("%1 GHz").arg(m_freqs[i], 0, 'f', 2));
                p.setFont(font);
            }
        }
    }
}

// ── CpuHistoryWidget ──────────────────────────────────────────────────────────

CpuHistoryWidget::CpuHistoryWidget(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(60);
}

void CpuHistoryWidget::updateCpu(const QList<double> &percpu)
{
    double avg = 0.0;
    if (!percpu.isEmpty()) {
        for (double v : percpu) avg += v;
        avg /= percpu.size();
    }
    VLOG("CpuHistoryWidget::updateCpu: avg=%.1f%% history=%lld widget %dx%d visible=%d",
         avg, (long long)(m_history.size() + 1), width(), height(), (int)isVisible());
    m_history.append(avg);
    if (m_history.size() > HISTORY_LEN) m_history.removeFirst();
    update();
}

void CpuHistoryWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int w = width(), h = height();
    p.setPen(Qt::NoPen); p.setBrush(BG);
    p.drawRect(0, 0, w, h);

    const int n = m_history.size();
    if (n >= 2) {
        const int offset = HISTORY_LEN - n;
        auto xAt = [&](int i) { return (int)(w * (offset+i) / (HISTORY_LEN-1)); };
        auto yAt = [&](double pct) { return h-2-(int)((h-4)*pct/100.0); };

        QPainterPath path;
        path.moveTo(xAt(0), h-2);
        path.lineTo(xAt(0), yAt(m_history[0]));
        for (int i = 1; i < n; ++i) path.lineTo(xAt(i), yAt(m_history[i]));
        path.lineTo(xAt(n-1), h-2);
        path.closeSubpath();

        QLinearGradient grad(0, 0, 0, h);
        QColor top = barColor(m_history.last()); top.setAlpha(180);
        QColor bot = top; bot.setAlpha(40);
        grad.setColorAt(0.0, top); grad.setColorAt(1.0, bot);
        p.setBrush(grad); p.drawPath(path);
    }

    p.setBrush(Qt::NoBrush); p.setPen(QPen(BORDER, 1));
    p.drawRect(0, 0, w-1, h-1);

    QFont lf; lf.setPixelSize(10); lf.setFamily(QStringLiteral("monospace"));
    p.setFont(lf);
    p.setPen(QColor(239, 240, 241, 180));
    p.drawText(4, 4, w-8, h-8, Qt::AlignTop | Qt::AlignLeft,
               QStringLiteral("CPU History (avg)"));
}
