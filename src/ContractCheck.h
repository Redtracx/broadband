#pragma once

#include "Report.h"

#include <QDate>
#include <QList>

// Pre-check against the Bundesnetzagentur rules for a reduced fixed-line
// speed (Minderung nach § 57 Abs. 4 TKG). Only the official Breitbandmessung
// counts legally; this tells whether it is worth doing.
//
// Protocol: 30 measurements on 3 calendar days (10 per day), at least 5 min
// between measurements and 3 h between the 5th and 6th of a day, at least one
// calendar day between the measurement days, all within 14 days.
//
// A significant deviation exists, per direction, if
//  1. 90 % of the maximum speed is not reached at least once on at least
//     two of the three days,
//  2. the normally available speed is not reached in 90 % of the measurements, or
//  3. the minimum speed is undercut at least once on at least two of the days.
namespace contract {

struct Speeds
{
    int maxMbps = 0;
    int normalMbps = 0;
    int minMbps = 0;

    bool isSet() const { return maxMbps > 0 || normalMbps > 0 || minMbps > 0; }
};

struct Contract
{
    Speeds download;
    Speeds upload;
};

constexpr int kDays = 3;
constexpr int kPerDay = 10;
constexpr int kWindowDays = 14;
constexpr int kMinSpacingMin = 5;
constexpr int kMidDayGapHours = 3;

struct Day
{
    QDate date;
    QList<SpeedRun> runs;
    QStringList protocolIssues; // deviations from the measurement protocol
};

struct Result
{
    QList<Day> days;           // the measurement days used, oldest first
    QStringList protocolIssues; // about the days as a whole
    QList<Finding> findings;    // verdict per criterion and direction
    bool complete = false;      // 3 days with 10 measurements each
    bool deviation = false;     // at least one criterion is met
};

// Uses the most recent measurement days within the 14-day window ending with
// the latest run. The runs should all be against the same internet server.
Result evaluate(const QList<SpeedRun> &runs, const Contract &contract);

} // namespace contract
