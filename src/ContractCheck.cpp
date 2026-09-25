#include "ContractCheck.h"

#include "Format.h"

#include <QCoreApplication>
#include <QLocale>

#include <algorithm>

namespace contract {

namespace {

QString tr(const char *text, int n = -1)
{
    return QCoreApplication::translate("ContractCheck", text, nullptr, n);
}

QString dateText(const QDate &date)
{
    return QLocale().toString(date, QStringLiteral("ddd dd.MM."));
}

void checkDirection(Result &result, const Speeds &speeds, const QString &direction,
                    double (*value)(const SpeedRun &))
{
    if (!speeds.isSet())
        return;
    const QString prefix = result.complete ? QString() : tr("Vorläufig: ");
    int total = 0;

    if (speeds.maxMbps > 0) {
        const double threshold = speeds.maxMbps * kContractThreshold;
        int daysReached = 0;
        for (const Day &day : std::as_const(result.days)) {
            if (std::any_of(day.runs.begin(), day.runs.end(),
                            [&](const SpeedRun &run) { return value(run) >= threshold; }))
                ++daysReached;
        }
        const int daysMissed = int(result.days.size()) - daysReached;
        const QString text = tr("%1: 90 % der Maximalgeschwindigkeit (%2 Mbit/s) an %3 von %4 Messtagen erreicht.")
                                 .arg(direction, fmt::mbps(threshold))
                                 .arg(daysReached)
                                 .arg(result.days.size());
        if (daysMissed >= 2) {
            result.deviation = true;
            result.findings << Finding{Finding::Bad, prefix + text + tr(" Nötig wären mindestens 2 Tage: "
                                                                        "erhebliche Abweichung.")};
        } else {
            result.findings << Finding{daysReached >= 2 ? Finding::Good : Finding::Info, prefix + text};
        }
    }

    for (const Day &day : std::as_const(result.days))
        total += int(day.runs.size());

    if (speeds.normalMbps > 0 && total > 0) {
        int reached = 0;
        for (const Day &day : std::as_const(result.days)) {
            reached += int(std::count_if(day.runs.begin(), day.runs.end(),
                                         [&](const SpeedRun &run) { return value(run) >= speeds.normalMbps; }));
        }
        const double share = double(reached) / double(total);
        const QString text = tr("%1: Normalgeschwindigkeit (%2 Mbit/s) in %3 von %4 Messungen erreicht (%5 %).")
                                 .arg(direction)
                                 .arg(speeds.normalMbps)
                                 .arg(reached)
                                 .arg(total)
                                 .arg(qRound(share * 100.0));
        if (share < 0.9) {
            result.deviation = true;
            result.findings << Finding{Finding::Bad, prefix + text + tr(" Nötig wären 90 %: erhebliche Abweichung.")};
        } else {
            result.findings << Finding{Finding::Good, prefix + text};
        }
    }

    if (speeds.minMbps > 0) {
        QStringList daysBelow;
        for (const Day &day : std::as_const(result.days)) {
            if (std::any_of(day.runs.begin(), day.runs.end(),
                            [&](const SpeedRun &run) { return value(run) < speeds.minMbps; }))
                daysBelow << dateText(day.date);
        }
        if (daysBelow.size() >= 2) {
            result.deviation = true;
            result.findings << Finding{Finding::Bad, prefix + tr("%1: Mindestgeschwindigkeit (%2 Mbit/s) an %3 "
                                                                 "Messtagen unterschritten (%4): erhebliche "
                                                                 "Abweichung.")
                                                                  .arg(direction)
                                                                  .arg(speeds.minMbps)
                                                                  .arg(daysBelow.size())
                                                                  .arg(daysBelow.join(QStringLiteral(", ")))};
        } else if (daysBelow.isEmpty()) {
            result.findings << Finding{Finding::Good, prefix + tr("%1: Mindestgeschwindigkeit (%2 Mbit/s) an keinem "
                                                                  "Messtag unterschritten.")
                                                                   .arg(direction)
                                                                   .arg(speeds.minMbps)};
        } else {
            result.findings << Finding{Finding::Warning, prefix + tr("%1: Mindestgeschwindigkeit (%2 Mbit/s) an "
                                                                     "einem Messtag unterschritten (%3).")
                                                                      .arg(direction)
                                                                      .arg(speeds.minMbps)
                                                                      .arg(daysBelow.first())};
        }
    }
}

} // namespace

Result evaluate(const QList<SpeedRun> &runs, const Contract &contract)
{
    Result result;
    if (runs.isEmpty())
        return result;

    QList<SpeedRun> sorted = runs;
    std::sort(sorted.begin(), sorted.end(), [](const SpeedRun &a, const SpeedRun &b) { return a.time < b.time; });

    // Measurement days, newest first: at least one calendar day between two
    // of them, all within 14 days of the newest.
    const QDate newest = sorted.last().time.date();
    QList<QDate> dates;
    QStringList skipped;
    for (auto it = sorted.crbegin(); it != sorted.crend(); ++it) {
        const QDate date = it->time.date();
        if (date.daysTo(newest) >= kWindowDays)
            break;
        if (!dates.isEmpty() && dates.last() == date)
            continue;
        if (dates.size() == kDays)
            break;
        if (!dates.isEmpty() && date.daysTo(dates.last()) < 2) {
            if (!skipped.contains(dateText(date)))
                skipped << dateText(date);
            continue;
        }
        dates << date;
    }
    std::reverse(dates.begin(), dates.end());
    if (!skipped.isEmpty()) {
        result.protocolIssues << tr("Nicht gewertet, weil direkt vor einem anderen Messtag: %1. Zwischen zwei "
                                    "Messtagen muss mindestens ein Kalendertag liegen.")
                                     .arg(skipped.join(QStringLiteral(", ")));
    }

    for (const QDate &date : std::as_const(dates)) {
        Day day;
        day.date = date;
        for (const SpeedRun &run : std::as_const(sorted)) {
            if (run.time.date() == date && day.runs.size() < kPerDay)
                day.runs << run;
        }
        if (day.runs.size() < kPerDay)
            day.protocolIssues << tr("%1 von %2 Messungen").arg(day.runs.size()).arg(kPerDay);
        for (qsizetype i = 1; i < day.runs.size(); ++i) {
            if (day.runs[i - 1].time.secsTo(day.runs[i].time) < kMinSpacingMin * 60) {
                day.protocolIssues << tr("Messungen %1 und %2 liegen weniger als %3 Minuten auseinander")
                                          .arg(i)
                                          .arg(i + 1)
                                          .arg(kMinSpacingMin);
                break;
            }
        }
        if (day.runs.size() > 5 && day.runs[4].time.secsTo(day.runs[5].time) < kMidDayGapHours * 3600) {
            day.protocolIssues << tr("zwischen der 5. und 6. Messung liegen weniger als %1 Stunden")
                                      .arg(kMidDayGapHours);
        }
        result.days << day;
    }

    result.complete = result.days.size() == kDays &&
                      std::all_of(result.days.begin(), result.days.end(),
                                  [](const Day &day) { return day.runs.size() == kPerDay; });

    int total = 0;
    for (const Day &day : std::as_const(result.days))
        total += int(day.runs.size());
    result.findings << Finding{result.complete ? Finding::Good : Finding::Info,
                               tr("%1 von %2 Messungen an %3 von %4 Messtagen.")
                                   .arg(total)
                                   .arg(kDays * kPerDay)
                                   .arg(result.days.size())
                                   .arg(kDays)};

    if (!contract.download.isSet() && !contract.upload.isSet()) {
        result.findings << Finding{Finding::Info, tr("Vertragswerte eintragen, um die Messungen zu bewerten. Sie "
                                                     "stehen im Produktinformationsblatt des Anbieters.")};
        return result;
    }

    checkDirection(result, contract.download, tr("Download"),
                   [](const SpeedRun &run) { return run.result.downloadMbps; });
    checkDirection(result, contract.upload, tr("Upload"), [](const SpeedRun &run) { return run.result.uploadMbps; });

    if (result.deviation) {
        result.findings << Finding{Finding::Warning,
                                   tr("Die Messungen deuten auf eine erhebliche Abweichung hin. Rechtlich zählt nur "
                                      "das Messprotokoll der offiziellen Breitbandmessung (breitbandmessung.de); "
                                      "diese Vorprüfung zeigt, dass sie sich lohnen dürfte.")};
    } else if (result.complete) {
        result.findings << Finding{Finding::Good,
                                   tr("Keine erhebliche Abweichung: Der Anschluss hält die Vertragswerte nach den "
                                      "Kriterien der Bundesnetzagentur ein.")};
    }
    return result;
}

} // namespace contract
