#pragma once

#include <QList>

#include <algorithm>
#include <cmath>

// Descriptive statistics for a series of repeated measurements: how much do
// the results vary, which runs are flukes, and is there a trend over time?
namespace stats {

struct Summary
{
    int count = 0;
    double mean = 0.0;
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
    double stddev = 0.0; // sample standard deviation
    double mad = 0.0;    // median absolute deviation

    // Coefficient of variation: spread relative to the mean (0.05 = 5 %).
    double cv() const { return mean > 0.0 ? stddev / mean : 0.0; }
};

inline double median(QList<double> values)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const qsizetype n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

inline Summary summarize(const QList<double> &values)
{
    Summary s;
    s.count = int(values.size());
    if (values.isEmpty())
        return s;

    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    s.min = *minIt;
    s.max = *maxIt;
    double sum = 0.0;
    for (double v : values)
        sum += v;
    s.mean = sum / double(s.count);
    s.median = median(values);

    if (s.count > 1) {
        double squares = 0.0;
        for (double v : values)
            squares += (v - s.mean) * (v - s.mean);
        s.stddev = std::sqrt(squares / double(s.count - 1));
    }

    QList<double> deviations;
    deviations.reserve(values.size());
    for (double v : values)
        deviations.append(std::abs(v - s.median));
    s.mad = median(deviations);
    return s;
}

// A single run is considered a fluke when it lies far away from the rest:
// more than 3 robust standard deviations (1.4826 * MAD) from the median.
// Deviations below 10 % of the median are never flagged, so a very steady
// series does not turn tiny wobbles into outliers. Needs at least 3 runs.
inline bool isOutlier(double value, const Summary &s)
{
    if (s.count < 3)
        return false;
    const double deviation = std::abs(value - s.median);
    return deviation > 3.0 * 1.4826 * s.mad && deviation > 0.1 * std::abs(s.median);
}

// The series without its outliers, to judge the spread of the regular runs.
inline QList<double> withoutOutliers(const QList<double> &values)
{
    const Summary s = summarize(values);
    QList<double> kept;
    for (double v : values) {
        if (!isOutlier(v, s))
            kept.append(v);
    }
    return kept;
}

enum class Stability { Unknown, VeryStable, Stable, Fluctuating, Unstable };

// Rating of a coefficient of variation.
inline Stability stability(double cv)
{
    return cv < 0.05 ? Stability::VeryStable : cv < 0.15 ? Stability::Stable
                                                          : cv < 0.3 ? Stability::Fluctuating
                                                                     : Stability::Unstable;
}

inline Stability stability(const Summary &s)
{
    if (s.count < 2 || s.mean <= 0.0)
        return Stability::Unknown;
    return stability(s.cv());
}

struct Trend
{
    double slope = 0.0;         // change per run
    double relativeChange = 0.0; // fitted change from first to last run relative to the mean
    double r2 = 0.0;            // how well a straight line explains the series (0..1)

    // Only a clear, consistent change of at least 10 % counts as a trend.
    bool significant() const { return std::abs(relativeChange) >= 0.1 && r2 >= 0.5; }
};

// Least-squares line through (run index, value). Needs at least 4 runs.
inline Trend trend(const QList<double> &values)
{
    Trend t;
    const qsizetype n = values.size();
    if (n < 4)
        return t;

    double meanX = double(n - 1) / 2.0;
    double meanY = 0.0;
    for (double v : values)
        meanY += v;
    meanY /= double(n);

    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        const double dx = double(i) - meanX;
        const double dy = values[i] - meanY;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    t.slope = sxy / sxx;
    t.r2 = syy > 0.0 ? (sxy * sxy) / (sxx * syy) : 0.0;
    t.relativeChange = meanY != 0.0 ? t.slope * double(n - 1) / meanY : 0.0;
    return t;
}

} // namespace stats
