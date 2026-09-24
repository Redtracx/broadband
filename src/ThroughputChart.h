#pragma once

#include <QColor>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QWidget>

// Lightweight live line chart (time in seconds vs. Mbit/s), drawn with
// QPainter so the app needs nothing beyond QtWidgets.
class ThroughputChart : public QWidget
{
    Q_OBJECT

public:
    explicit ThroughputChart(QWidget *parent = nullptr);

    int addSeries(const QString &name, const QColor &color);
    void reset(double durationSec);
    void addSample(int series, double seconds, double mbps);
    // Dashed horizontal line, e.g. the contracted bandwidth (0 hides it).
    void setReferenceLine(double mbps, const QString &label);
    // Dashed vertical line with a label, e.g. the start of a load stage.
    void addMarker(double seconds, const QString &label);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Series
    {
        QString name;
        QColor color;
        QList<QPointF> points;
    };

    static double niceCeil(double value);

    QList<Series> m_series;
    QList<QPair<double, QString>> m_markers;
    double m_reference = 0.0;
    QString m_referenceLabel;
    double m_duration = 10.0;
    double m_maxY = 0.0;
};
