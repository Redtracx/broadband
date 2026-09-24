#pragma once

#include <QLocale>
#include <QString>

namespace fmt {

// Throughput in Mbit/s with a precision that fits the magnitude.
inline QString mbps(double value)
{
    const int decimals = value < 10.0 ? 2 : value < 100.0 ? 1 : 0;
    return QLocale().toString(value, 'f', decimals);
}

inline QString ms(double value)
{
    return QLocale().toString(value, 'f', value < 10.0 ? 1 : 0);
}

inline QString bytes(qint64 value)
{
    return QLocale().formattedDataSize(value, 1, QLocale::DataSizeSIFormat);
}

} // namespace fmt
