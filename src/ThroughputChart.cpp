#include "ThroughputChart.h"

#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

ThroughputChart::ThroughputChart(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

int ThroughputChart::addSeries(const QString &name, const QColor &color)
{
    m_series.append(Series{name, color, {}});
    update();
    return int(m_series.size() - 1);
}

void ThroughputChart::reset(double durationSec)
{
    for (Series &series : m_series)
        series.points.clear();
    m_markers.clear();
    m_duration = std::max(1.0, durationSec);
    m_maxY = 0.0;
    update();
}

void ThroughputChart::addSample(int series, double seconds, double mbps)
{
    if (series < 0 || series >= m_series.size())
        return;
    m_series[series].points.append({seconds, mbps});
    m_maxY = std::max(m_maxY, mbps);
    update();
}

void ThroughputChart::setReferenceLine(double mbps, const QString &label)
{
    m_reference = std::max(0.0, mbps);
    m_referenceLabel = label;
    update();
}

void ThroughputChart::addMarker(double seconds, const QString &label)
{
    m_markers.append({seconds, label});
    update();
}

QSize ThroughputChart::minimumSizeHint() const
{
    return {320, 160};
}

QSize ThroughputChart::sizeHint() const
{
    return {640, 240};
}

double ThroughputChart::niceCeil(double value)
{
    if (value <= 0.0)
        return 10.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
    for (const double step : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (step * magnitude >= value)
            return step * magnitude;
    }
    return 10.0 * magnitude;
}

void ThroughputChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette pal = palette();
    const QFontMetrics fm = fontMetrics();
    const QColor textColor = pal.color(QPalette::WindowText);
    QColor gridColor = textColor;
    gridColor.setAlpha(40);

    painter.fillRect(rect(), pal.color(QPalette::Base));

    const double maxY = niceCeil(std::max(m_maxY, m_reference) * 1.1);
    const int left = fm.horizontalAdvance(QStringLiteral("00000")) + 8;
    const QRectF plot(left, fm.height() + 8, width() - left - 12, height() - 2 * fm.height() - 16);
    if (plot.width() < 10 || plot.height() < 10)
        return;

    auto mapPoint = [&](const QPointF &p) {
        return QPointF(plot.left() + p.x() / m_duration * plot.width(),
                       plot.bottom() - std::min(p.y(), maxY) / maxY * plot.height());
    };

    // Horizontal grid with Mbit/s labels
    constexpr int kRows = 4;
    painter.setFont(font());
    for (int i = 0; i <= kRows; ++i) {
        const double value = maxY * i / kRows;
        const double y = plot.bottom() - plot.height() * i / kRows;
        painter.setPen(gridColor);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(textColor);
        const QString label = QLocale().toString(value, 'f', value < 10.0 && value > 0.0 ? 1 : 0);
        painter.drawText(QRectF(0, y - fm.height() / 2.0, left - 6, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, label);
    }

    // Time axis
    const double xStep = m_duration <= 10 ? 1.0 : m_duration <= 30 ? 5.0 : m_duration <= 120 ? 10.0 : 30.0;
    for (double t = 0.0; t <= m_duration + 1e-9; t += xStep) {
        const double x = plot.left() + t / m_duration * plot.width();
        painter.setPen(gridColor);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(textColor);
        painter.drawText(QRectF(x - 20, plot.bottom() + 4, 40, fm.height()), Qt::AlignCenter,
                         QStringLiteral("%1 s").arg(t, 0, 'f', 0));
    }

    painter.setPen(textColor);
    painter.drawText(QRectF(0, 0, left + 60, fm.height() + 4), Qt::AlignLeft | Qt::AlignTop,
                     tr("Mbit/s"));

    // Stage markers
    QPen markerPen(textColor, 1.0, Qt::DashLine);
    for (const auto &[seconds, label] : m_markers) {
        const double x = plot.left() + seconds / m_duration * plot.width();
        painter.setPen(markerPen);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(textColor);
        painter.drawText(QRectF(x + 3, plot.top() + 2, 80, fm.height()), Qt::AlignLeft | Qt::AlignTop, label);
    }

    // Reference line (e.g. contracted bandwidth)
    if (m_reference > 0.0) {
        const QColor referenceColor(0x2e, 0x9d, 0x4f);
        const double y = plot.bottom() - m_reference / maxY * plot.height();
        painter.setPen(QPen(referenceColor, 1.5, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(referenceColor);
        painter.drawText(QRectF(plot.left(), y - fm.height() - 1, plot.width() - 4, fm.height()),
                         Qt::AlignRight | Qt::AlignBottom, m_referenceLabel);
    }

    // Series
    for (const Series &series : m_series) {
        if (series.points.isEmpty())
            continue;

        QPainterPath line;
        line.moveTo(mapPoint({0.0, series.points.first().y()}));
        for (const QPointF &p : series.points)
            line.lineTo(mapPoint(p));

        QPainterPath area = line;
        area.lineTo(mapPoint({series.points.last().x(), 0.0}));
        area.lineTo(mapPoint({0.0, 0.0}));
        area.closeSubpath();

        QColor fill = series.color;
        fill.setAlpha(45);
        painter.fillPath(area, fill);
        painter.setPen(QPen(series.color, 2.0));
        painter.drawPath(line);
    }

    // Legend (top right)
    double x = plot.right();
    for (auto it = m_series.crbegin(); it != m_series.crend(); ++it) {
        const int textWidth = fm.horizontalAdvance(it->name);
        x -= textWidth;
        painter.setPen(textColor);
        painter.drawText(QRectF(x, 2, textWidth, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, it->name);
        x -= 16;
        painter.fillRect(QRectF(x, 2 + fm.height() / 2.0 - 5, 10, 10), it->color);
        x -= 16;
    }
}
