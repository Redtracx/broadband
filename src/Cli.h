#pragma once

#include "HttpCommon.h"

#include <QList>
#include <QUrl>

class QCoreApplication;

// Measurements without a GUI, e.g. for cron jobs:
//   broadband --run --format csv
//   broadband --load 1,4,16 --contract 250
struct CliOptions
{
    enum class Format { Text, Json, Csv };

    QUrl url{QStringLiteral("https://speed.cloudflare.com")};
    QList<int> loadStages; // empty: speed test
    int durationSec = 10;  // per direction, or per stage for the load test
    int streams = 4;
    bool upload = false;   // load test direction
    int contractMbps = 0;
    http::IpVersion ipVersion = http::IpVersion::Any;
    Format format = Format::Text;
    bool header = true; // CSV header line
    bool save = true;   // append to the history file
};

int runCli(QCoreApplication &app, const CliOptions &options);
