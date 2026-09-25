#include "MainWindow.h"

#include "Format.h"
#include "History.h"
#include "TestServer.h"
#include "ThroughputChart.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

const QColor kDownloadColor(0x2f, 0x80, 0xed);
const QColor kUploadColor(0x9b, 0x51, 0xe0);
const QColor kGoodColor(0x2e, 0x9d, 0x4f);
const QColor kWarnColor(0xd9, 0x82, 0x1e);
const QColor kBadColor(0xd6, 0x3b, 0x3b);
const QString kDefaultServer = QStringLiteral("https://speed.cloudflare.com");

// Share of the overall progress bar per speed test phase.
constexpr double kLatencyWeight = 0.1;
constexpr double kTransferWeight = 0.45;

QString phaseText(SpeedTest::Phase phase)
{
    switch (phase) {
    case SpeedTest::Phase::Latency:
        return MainWindow::tr("Messe Latenz …");
    case SpeedTest::Phase::Download:
        return MainWindow::tr("Messe Download …");
    case SpeedTest::Phase::Upload:
        return MainWindow::tr("Messe Upload …");
    case SpeedTest::Phase::Done:
        return MainWindow::tr("Fertig");
    case SpeedTest::Phase::Idle:
        break;
    }
    return MainWindow::tr("Bereit");
}

QTableWidgetItem *cell(const QString &text, Qt::Alignment alignment = Qt::AlignRight | Qt::AlignVCenter)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    return item;
}

QTableWidget *createTable(const QStringList &headers)
{
    auto *table = new QTableWidget(0, int(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setMinimumHeight(110);
    return table;
}

QColor ratingColor(double fraction)
{
    return fraction >= kContractThreshold ? kGoodColor : fraction >= 0.5 ? kWarnColor : kBadColor;
}

QString colored(const QString &text, const QColor &color)
{
    return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color.name(), text);
}

// Findings as colored lines for a rich text label.
QString findingsHtml(const QList<Finding> &findings)
{
    QStringList lines;
    for (const Finding &finding : findings) {
        const QString text = finding.text.toHtmlEscaped();
        switch (finding.level) {
        case Finding::Info:
            lines << QStringLiteral("• ") + text;
            break;
        case Finding::Good:
            lines << colored(QStringLiteral("✔ ") + text, kGoodColor);
            break;
        case Finding::Warning:
            lines << colored(QStringLiteral("⚠ ") + text, kWarnColor);
            break;
        case Finding::Bad:
            lines << colored(QStringLiteral("<b>✘ ") + text + QStringLiteral("</b>"), kBadColor);
            break;
        }
    }
    return lines.join(QStringLiteral("<br>"));
}

QString loadedLatencyText(const SpeedTestResult &r)
{
    const auto value = [](double ms) { return ms < 0 ? QStringLiteral("–") : fmt::ms(ms); };
    if (r.downloadLatencyMs < 0 && r.uploadLatencyMs < 0)
        return QStringLiteral("–");
    return QStringLiteral("%1 / %2").arg(value(r.downloadLatencyMs), value(r.uploadLatencyMs));
}

QString timeText(const QDateTime &time)
{
    return time.date() == QDate::currentDate() ? time.toString(QStringLiteral("HH:mm:ss"))
                                               : QLocale().toString(time, QStringLiteral("dd.MM.yy HH:mm"));
}

// "https://speed.cloudflare.com · FRA · IPv4"
QString serverText(const SpeedRun &run)
{
    QStringList parts{run.server};
    if (!run.result.server.colo.isEmpty())
        parts << run.result.server.colo;
    if (run.ipVersion)
        parts << QStringLiteral("IPv%1").arg(run.ipVersion);
    return parts.join(QStringLiteral(" · "));
}

void addLoadRow(QTableWidget *table, int run, const LoadStageResult &result, int contractMbps)
{
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, cell(QString::number(run), Qt::AlignCenter));
    table->setItem(row, 1, cell(QString::number(result.clients)));
    table->setItem(row, 2, cell(fmt::mbps(result.totalMbps)));

    auto *contractCell = cell(QStringLiteral("–"));
    if (contractMbps > 0) {
        const double fraction = result.totalMbps / contractMbps;
        contractCell->setText(QStringLiteral("%1 %").arg(qRound(fraction * 100)));
        contractCell->setForeground(ratingColor(fraction));
        QFont font = contractCell->font();
        font.setBold(true);
        contractCell->setFont(font);
    }
    table->setItem(row, 3, contractCell);

    table->setItem(row, 4, cell(fmt::mbps(result.clientAvgMbps)));
    table->setItem(row, 5, cell(QStringLiteral("%1 – %2").arg(fmt::mbps(result.clientMinMbps),
                                                              fmt::mbps(result.clientMaxMbps))));
    table->setItem(row, 6, cell(result.latencyMs < 0 ? QStringLiteral("–")
                                                     : MainWindow::tr("%1 ms").arg(fmt::ms(result.latencyMs))));
    table->setItem(row, 7, cell(fmt::bytes(result.bytes)));
    if (result.retries > 0) {
        for (int column = 0; column < table->columnCount(); ++column)
            table->item(row, column)->setToolTip(
                MainWindow::tr("%n Übertragung(en) abgebrochen und neu gestartet", nullptr, result.retries));
    }
}

// Internet measurements only: the contract check makes no sense for LAN tests.
QList<SpeedRun> internetRuns(const QList<SpeedRun> &runs)
{
    QList<SpeedRun> result;
    for (const SpeedRun &run : runs) {
        if (http::proxyFor(QUrl(run.server)).type() != QNetworkProxy::NoProxy)
            result << run;
    }
    return result;
}

// "1, 2, 4, 8" -> {1, 2, 4, 8}; empty list on invalid input
QList<int> parseStages(const QString &text)
{
    QList<int> stages;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 1 || value > 64)
            return {};
        stages.append(value);
    }
    return stages;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_test(new SpeedTest(this))
    , m_load(new LoadTest(this))
    , m_server(new TestServer(this))
{
    setWindowTitle(tr("Broadband – Bandbreiten- & Speedtest"));

    // The server address is shared by the speed test and the load test.
    m_serverUrl = new QComboBox;
    m_serverUrl->setEditable(true);
    m_serverUrl->addItem(kDefaultServer);
    m_serverUrl->addItem(QStringLiteral("http://localhost:8080"));
    m_serverUrl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_serverUrl->setToolTip(tr("Cloudflare für Messungen des Internetanschlusses oder die Adresse eines "
                               "Rechners, auf dem der LAN-Server läuft."));
    m_ipVersion = new QComboBox;
    m_ipVersion->addItem(tr("IPv4 + IPv6"), int(http::IpVersion::Any));
    m_ipVersion->addItem(tr("nur IPv4"), int(http::IpVersion::V4));
    m_ipVersion->addItem(tr("nur IPv6"), int(http::IpVersion::V6));
    m_ipVersion->setToolTip(tr("Über IPv4 und IPv6 kann der Weg zum Server unterschiedlich schnell sein. "
                               "Hinter einem Proxy lässt sich die Version nicht erzwingen."));

    m_serverInfo = new QLabel;
    m_serverInfo->setEnabled(false); // dimmed secondary text
    m_serverInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(new QLabel(tr("Server:")));
    serverRow->addWidget(m_serverUrl, 1);
    serverRow->addWidget(m_ipVersion);

    m_tabs = new QTabWidget;
    m_tabs->addTab(createSpeedTestTab(), tr("Speedtest"));
    m_tabs->addTab(createLoadTestTab(), tr("Bandbreite unter Last"));
    m_tabs->addTab(createContractTab(), tr("Vertrag prüfen"));
    m_tabs->addTab(createServerTab(), tr("LAN-Server"));

    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->addLayout(serverRow);
    centralLayout->addWidget(m_serverInfo);
    centralLayout->addWidget(m_tabs, 1);
    setCentralWidget(central);

    connect(m_test, &SpeedTest::phaseChanged, this, &MainWindow::onPhaseChanged);
    connect(m_test, &SpeedTest::progressChanged, this, &MainWindow::onProgress);
    connect(m_test, &SpeedTest::latencyMeasured, this, [this](double ping, double jitter) {
        m_pingTile.value->setText(fmt::ms(ping));
        m_pingTile.detail->setText(tr("ms · Jitter %1 ms").arg(fmt::ms(jitter)));
    });
    connect(m_test, &SpeedTest::serverInfoChanged, this, &MainWindow::showServerInfo);
    connect(m_test, &SpeedTest::throughputSample, this, &MainWindow::onSample);
    connect(m_test, &SpeedTest::phaseFinished, this, &MainWindow::onPhaseFinished);
    connect(m_test, &SpeedTest::finished, this, &MainWindow::onFinished);
    // Queued: the message box must not spin an event loop inside a network callback.
    connect(m_test, &SpeedTest::failed, this, &MainWindow::onFailed, Qt::QueuedConnection);

    connect(m_load, &LoadTest::idleLatencyMeasured, this, [this](double ms) {
        m_loadStatus->setText(tr("Ping ohne Last: %1 ms").arg(fmt::ms(ms)));
    });
    // With several runs the chart shows them back to back.
    auto runOffset = [this] { return (m_loadRun - 1) * LoadTest::totalDuration(m_loadConfig); };
    connect(m_load, &LoadTest::stageStarted, this, [this, runOffset](int index, int clients) {
        const QString label = index == 0 && m_loadRun > 1 ? tr("Lauf %1").arg(m_loadRun) : QStringLiteral("%1×").arg(clients);
        m_loadChart->addMarker(runOffset() + index * double(m_loadConfig.stageDurationSec), label);
        QString status = tr("Stufe %1: %n gleichzeitige(r) Client(s) …", nullptr, clients).arg(index + 1);
        if (m_loadRunTotal > 1)
            status = tr("Durchlauf %1/%2 · ").arg(m_loadRun).arg(m_loadRunTotal) + status;
        m_loadStatus->setText(status);
    });
    connect(m_load, &LoadTest::sample, this, [this, runOffset](double seconds, double mbps) {
        m_loadChart->addSample(m_loadSeriesIndex, runOffset() + seconds, mbps);
    });
    connect(m_load, &LoadTest::progressChanged, this, [this](double fraction) {
        const double overall = (m_loadRun - 1 + fraction) / std::max(1, m_loadRunTotal);
        m_loadProgress->setValue(int(overall * m_loadProgress->maximum()));
    });
    connect(m_load, &LoadTest::stageFinished, this, &MainWindow::onLoadStageFinished);
    connect(m_load, &LoadTest::finished, this, &MainWindow::onLoadFinished);
    connect(m_load, &LoadTest::stopped, this, [this] {
        m_loadSeriesActive = false;
        m_loadStatus->setText(tr("Abgebrochen"));
        updateRunningState();
    });
    connect(
        m_load, &LoadTest::failed, this,
        [this](const QString &message) {
            m_loadSeriesActive = false;
            m_loadStatus->setText(tr("Fehler"));
            updateRunningState();
            QMessageBox::warning(this, windowTitle(), message);
        },
        Qt::QueuedConnection);

    m_pauseTimer.setInterval(1000);
    connect(&m_pauseTimer, &QTimer::timeout, this, &MainWindow::pauseTick);

    connect(m_server, &TestServer::connectionCountChanged, this, &MainWindow::updateServerStatus);
    m_serverStatsTimer.setInterval(500);
    connect(&m_serverStatsTimer, &QTimer::timeout, this, &MainWindow::updateServerStatus);

    loadSettings();
    m_historyPath = history::defaultPath();
    loadHistory();
    updateContractLine();
    updateContractCheck();
    updateServerStatus();
    updateRunningState();
    resize(860, 820);
}

void MainWindow::setServerUrl(const QString &url)
{
    m_serverUrl->setCurrentText(url);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_pauseTimer.stop();
    m_speedSeriesActive = false;
    m_loadSeriesActive = false;
    m_test->stop();
    m_load->stop();
    saveSettings();
    event->accept();
}

// --- UI construction --------------------------------------------------------

QWidget *MainWindow::createTile(const QString &title, const QString &unit, Tile &tile)
{
    auto *frame = new QFrame;
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setMinimumWidth(160);

    auto *titleLabel = new QLabel(title);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    tile.value = new QLabel(QStringLiteral("–"));
    QFont valueFont = tile.value->font();
    valueFont.setPointSizeF(valueFont.pointSizeF() * 2.6);
    valueFont.setBold(true);
    tile.value->setFont(valueFont);

    tile.detail = new QLabel(unit);
    tile.detail->setEnabled(false); // dimmed secondary text

    auto *layout = new QVBoxLayout(frame);
    layout->addWidget(titleLabel);
    layout->addWidget(tile.value);
    layout->addWidget(tile.detail);
    return frame;
}

QWidget *MainWindow::createSpeedTestTab()
{
    auto *page = new QWidget;

    // Result tiles
    auto *tiles = new QHBoxLayout;
    tiles->addWidget(createTile(tr("Ping"), tr("ms"), m_pingTile));
    tiles->addWidget(createTile(tr("Download"), tr("Mbit/s"), m_downloadTile));
    tiles->addWidget(createTile(tr("Upload"), tr("Mbit/s"), m_uploadTile));

    QPalette downloadPalette = m_downloadTile.value->palette();
    downloadPalette.setColor(QPalette::WindowText, kDownloadColor);
    m_downloadTile.value->setPalette(downloadPalette);
    QPalette uploadPalette = m_uploadTile.value->palette();
    uploadPalette.setColor(QPalette::WindowText, kUploadColor);
    m_uploadTile.value->setPalette(uploadPalette);

    // Live chart
    m_chart = new ThroughputChart;
    m_downloadSeries = m_chart->addSeries(tr("Download"), kDownloadColor);
    m_uploadSeries = m_chart->addSeries(tr("Upload"), kUploadColor);

    // Settings
    m_duration = new QSpinBox;
    m_duration->setRange(3, 60);
    m_duration->setValue(10);
    m_duration->setSuffix(tr(" s"));
    m_duration->setToolTip(tr("Messdauer je Richtung (Download und Upload)"));

    m_streams = new QSpinBox;
    m_streams->setRange(1, SpeedTest::kMaxStreams);
    m_streams->setValue(4);
    m_streams->setToolTip(tr("Anzahl paralleler Verbindungen. Mehrere Streams lasten schnelle "
                             "Leitungen besser aus, ab etwa 1 Gbit/s z. B. 8 bis 16."));

    m_repeats = new QSpinBox;
    m_repeats->setRange(1, 100);
    m_repeats->setValue(1);
    m_repeats->setSuffix(tr(" ×"));
    m_repeats->setToolTip(tr("Mehrere Messungen nacheinander. Ab 3 Messungen lassen sich Ausreißer erkennen, "
                             "ab 4 ein Trend."));

    m_pause = new QSpinBox;
    m_pause->setRange(0, 3600);
    m_pause->setValue(30);
    m_pause->setSuffix(tr(" s"));
    m_pause->setToolTip(tr("Wartezeit zwischen zwei Messungen. Mit längeren Pausen, z. B. 600 s, lässt sich "
                           "die Leitung über Stunden beobachten."));
    connect(m_repeats, &QSpinBox::valueChanged, this, &MainWindow::updateRunningState);

    m_startButton = new QPushButton(tr("Test starten"));
    m_startButton->setDefault(true);
    m_startButton->setMinimumHeight(40);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::toggleTest);

    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Dauer:")));
    controls->addWidget(m_duration);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Streams:")));
    controls->addWidget(m_streams);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Wiederholungen:")));
    controls->addWidget(m_repeats);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Pause:")));
    controls->addWidget(m_pause);
    controls->addStretch();
    controls->addWidget(m_startButton);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);

    m_status = new QLabel(phaseText(SpeedTest::Phase::Idle));

    // History of this session
    m_history = createTable({tr("Serie"), tr("Zeit"), tr("Server"), tr("Ping (ms)"), tr("Jitter (ms)"),
                             tr("Download (Mbit/s)"), tr("Upload (Mbit/s)"), tr("Ping unter Last ↓ / ↑")});
    m_history->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_history->setToolTip(tr("Rot markierte Werte weichen deutlich vom Rest ihrer Serie ab (Ausreißer). "
                             "Der Verlauf bleibt zwischen den Starts erhalten."));

    m_clearButton = new QPushButton(tr("Verlauf leeren"));
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::clearHistory);
    m_exportButton = new QPushButton(tr("Excel-Export …"));
    m_exportButton->setToolTip(tr("Alle Messungen des Verlaufs mit Auswertung als Excel-Datei (.xlsx) speichern"));
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::exportReport);

    auto *historyRow = new QHBoxLayout;
    historyRow->addWidget(new QLabel(tr("Verlauf")));
    historyRow->addStretch();
    historyRow->addWidget(m_clearButton);
    historyRow->addWidget(m_exportButton);

    m_seriesAnalysis = new QLabel;
    m_seriesAnalysis->setWordWrap(true);
    m_seriesAnalysis->setTextFormat(Qt::RichText);
    m_seriesAnalysis->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(tiles);
    layout->addWidget(m_chart, 1);
    layout->addLayout(controls);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addLayout(historyRow);
    layout->addWidget(m_history);
    layout->addWidget(m_seriesAnalysis);
    return page;
}

QWidget *MainWindow::createLoadTestTab()
{
    auto *page = new QWidget;

    auto *intro = new QLabel(
        tr("Prüft, ob die vertragliche Bandbreite auch dann ankommt, wenn mehrere Geräte gleichzeitig laden. "
           "Jede Stufe simuliert die angegebene Zahl gleichzeitiger Clients mit jeweils eigener Verbindung "
           "und misst Gesamtdurchsatz, Anteil pro Client und den Ping unter Last."));
    intro->setWordWrap(true);

    m_contract = new QSpinBox;
    m_contract->setRange(0, 100'000);
    m_contract->setSuffix(tr(" Mbit/s"));
    m_contract->setSpecialValueText(tr("unbekannt"));
    m_contract->setToolTip(tr("Vertraglich vereinbarte (maximale) Geschwindigkeit in dieser Richtung"));
    connect(m_contract, &QSpinBox::valueChanged, this, &MainWindow::updateContractLine);

    m_loadDirection = new QComboBox;
    m_loadDirection->addItem(tr("Download"));
    m_loadDirection->addItem(tr("Upload"));

    m_loadStages = new QLineEdit(QStringLiteral("1, 2, 4, 8, 16"));
    m_loadStages->setToolTip(tr("Anzahl gleichzeitiger Clients je Stufe, durch Komma getrennt (1–64)"));

    m_loadDuration = new QSpinBox;
    m_loadDuration->setRange(5, 600);
    m_loadDuration->setValue(10);
    m_loadDuration->setSuffix(tr(" s"));
    m_loadDuration->setToolTip(tr("Messdauer je Stufe. Für einen Dauertest einfach hoch setzen, "
                                  "z. B. eine Stufe mit 300 s."));

    m_loadRuns = new QSpinBox;
    m_loadRuns->setRange(1, 20);
    m_loadRuns->setValue(1);
    m_loadRuns->setSuffix(tr(" ×"));
    m_loadRuns->setToolTip(tr("Den kompletten Test mehrmals nacheinander ausführen. Die Auswertung nutzt dann "
                              "den Median je Stufe und zeigt, ob die Durchläufe übereinstimmen."));
    connect(m_loadRuns, &QSpinBox::valueChanged, this, &MainWindow::updateRunningState);

    m_loadPause = new QSpinBox;
    m_loadPause->setRange(0, 3600);
    m_loadPause->setValue(30);
    m_loadPause->setSuffix(tr(" s"));
    m_loadPause->setToolTip(tr("Wartezeit zwischen zwei Durchläufen"));

    auto *runsRow = new QHBoxLayout;
    runsRow->addWidget(m_loadRuns);
    runsRow->addSpacing(16);
    runsRow->addWidget(new QLabel(tr("Pause dazwischen:")));
    runsRow->addWidget(m_loadPause);
    runsRow->addStretch();

    auto *form = new QFormLayout;
    form->addRow(tr("Vertrag:"), m_contract);
    form->addRow(tr("Richtung:"), m_loadDirection);
    form->addRow(tr("Gleichzeitige Clients:"), m_loadStages);
    form->addRow(tr("Dauer je Stufe:"), m_loadDuration);
    form->addRow(tr("Durchläufe:"), runsRow);

    m_loadButton = new QPushButton(tr("Test starten"));
    m_loadButton->setMinimumHeight(40);
    connect(m_loadButton, &QPushButton::clicked, this, &MainWindow::toggleLoadTest);

    m_loadExportButton = new QPushButton(tr("Excel-Export …"));
    m_loadExportButton->setToolTip(m_exportButton->toolTip());
    connect(m_loadExportButton, &QPushButton::clicked, this, &MainWindow::exportReport);

    auto *buttons = new QVBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_loadExportButton);
    buttons->addWidget(m_loadButton);

    auto *settingsRow = new QHBoxLayout;
    settingsRow->addLayout(form, 1);
    settingsRow->addLayout(buttons);

    m_loadChart = new ThroughputChart;
    m_loadSeriesIndex = m_loadChart->addSeries(tr("Gesamt"), kDownloadColor);

    m_loadProgress = new QProgressBar;
    m_loadProgress->setRange(0, 1000);
    m_loadProgress->setValue(0);
    m_loadProgress->setTextVisible(false);

    m_loadStatus = new QLabel(tr("Bereit"));

    m_loadTable = createTable({tr("Lauf"), tr("Clients"), tr("Gesamt (Mbit/s)"), tr("vom Vertrag"), tr("Ø je Client"),
                               tr("Min – Max je Client"), tr("Ping unter Last"), tr("Datenmenge")});

    m_loadSummary = new QLabel;
    m_loadSummary->setWordWrap(true);
    m_loadSummary->setTextFormat(Qt::RichText);
    m_loadSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(intro);
    layout->addLayout(settingsRow);
    layout->addWidget(m_loadChart, 1);
    layout->addWidget(m_loadProgress);
    layout->addWidget(m_loadStatus);
    layout->addWidget(m_loadTable);
    layout->addWidget(m_loadSummary);
    return page;
}

QWidget *MainWindow::createContractTab()
{
    auto *page = new QWidget;

    auto *intro = new QLabel(
        tr("Vorprüfung nach den Kriterien der Bundesnetzagentur für eine Minderung (§ 57 TKG): 30 Messungen an "
           "3 Kalendertagen, je Tag 10 Messungen mit mindestens 5 Minuten Abstand und 3 Stunden Pause nach der "
           "5. Messung, mindestens ein Tag zwischen den Messtagen, alles innerhalb von 14 Tagen. Ausgewertet werden "
           "die Speedtests gegen Internet-Server aus dem Verlauf. Rechtlich zählt nur die offizielle "
           "<a href=\"https://breitbandmessung.de\">Breitbandmessung</a>; diese Prüfung zeigt, ob sie sich lohnt."));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    intro->setOpenExternalLinks(true);

    auto speedBox = [this](SpeedInputs &inputs) {
        auto make = [this](const QString &tip) {
            auto *box = new QSpinBox;
            box->setRange(0, 100'000);
            box->setSuffix(tr(" Mbit/s"));
            box->setSpecialValueText(tr("–"));
            box->setToolTip(tip);
            connect(box, &QSpinBox::valueChanged, this, &MainWindow::updateContractCheck);
            return box;
        };
        inputs.max = make(tr("Vertraglich vereinbarte maximale Geschwindigkeit"));
        inputs.normal = make(tr("Normalerweise zur Verfügung stehende Geschwindigkeit"));
        inputs.min = make(tr("Minimale Geschwindigkeit"));
    };
    speedBox(m_contractDown);
    speedBox(m_contractUp);

    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Maximal")), 0, 1);
    grid->addWidget(new QLabel(tr("Normal")), 0, 2);
    grid->addWidget(new QLabel(tr("Minimal")), 0, 3);
    grid->addWidget(new QLabel(tr("Download:")), 1, 0);
    grid->addWidget(m_contractDown.max, 1, 1);
    grid->addWidget(m_contractDown.normal, 1, 2);
    grid->addWidget(m_contractDown.min, 1, 3);
    grid->addWidget(new QLabel(tr("Upload:")), 2, 0);
    grid->addWidget(m_contractUp.max, 2, 1);
    grid->addWidget(m_contractUp.normal, 2, 2);
    grid->addWidget(m_contractUp.min, 2, 3);
    grid->setColumnStretch(4, 1);

    auto *box = new QGroupBox(tr("Vertrag (siehe Produktinformationsblatt)"));
    box->setLayout(grid);

    m_blockButton = new QPushButton(tr("Messblock starten (5 × alle 5 min)"));
    m_blockButton->setToolTip(tr("Startet im Speedtest-Tab 5 Messungen mit 5 Minuten Pause. Zwei solche Blöcke "
                                 "im Abstand von mindestens 3 Stunden ergeben einen Messtag."));
    connect(m_blockButton, &QPushButton::clicked, this, &MainWindow::startMeasurementBlock);

    auto *hint = new QLabel(
        tr("Ohne offenes Fenster geht es mit einem cron-Job, z. B. um 9, 9:10, … und ab 18 Uhr:<br>"
           "<code>0,10,20,30,40 9,18 * * * broadband --run --format csv --no-header &gt;&gt; ~/speed.csv</code>"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    hint->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_contractDays = createTable({tr("Messtag"), tr("Messungen"), tr("Download (Mbit/s)"), tr("Upload (Mbit/s)"),
                                  tr("Protokoll")});
    m_contractDays->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    m_contractSummary = new QLabel;
    m_contractSummary->setWordWrap(true);
    m_contractSummary->setTextFormat(Qt::RichText);
    m_contractSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_blockButton);
    buttons->addStretch();

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(intro);
    layout->addWidget(box);
    layout->addLayout(buttons);
    layout->addWidget(hint);
    layout->addWidget(m_contractDays, 1);
    layout->addWidget(m_contractSummary);
    return page;
}

QWidget *MainWindow::createServerTab()
{
    auto *page = new QWidget;

    auto *intro = new QLabel(
        tr("Um die Bandbreite zwischen zwei Rechnern im lokalen Netz zu messen, startest du hier "
           "den Testserver und trägst auf dem anderen Rechner eine der unten angezeigten Adressen "
           "als Server ein. Alternativ läuft der Server auch ohne GUI: <code>broadband --server</code>"));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);

    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(8080);

    m_serverButton = new QPushButton(tr("Server starten"));
    connect(m_serverButton, &QPushButton::clicked, this, &MainWindow::toggleServer);

    m_useLocalButton = new QPushButton(tr("Als Testziel verwenden"));
    m_useLocalButton->setToolTip(tr("Diesen Rechner als Server eintragen (Loopback-Test der eigenen Maschine)."));
    connect(m_useLocalButton, &QPushButton::clicked, this, [this] {
        setServerUrl(QStringLiteral("http://localhost:%1").arg(m_server->port()));
        m_tabs->setCurrentIndex(0);
    });

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_serverButton);
    buttons->addWidget(m_useLocalButton);
    buttons->addStretch();

    m_serverState = new QLabel;
    m_serverAddresses = new QLabel;
    m_serverAddresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_serverTraffic = new QLabel;

    auto *box = new QGroupBox(tr("Testserver"));
    auto *form = new QFormLayout(box);
    form->addRow(tr("Port:"), m_port);
    form->addRow(buttons);
    form->addRow(tr("Status:"), m_serverState);
    form->addRow(tr("Adressen:"), m_serverAddresses);
    form->addRow(tr("Traffic:"), m_serverTraffic);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(intro);
    layout->addWidget(box);
    layout->addStretch();
    return page;
}

// --- Settings ---------------------------------------------------------------

void MainWindow::loadSettings()
{
    QSettings settings;
    const QString url = settings.value(QStringLiteral("test/server"), kDefaultServer).toString();
    if (m_serverUrl->findText(url) < 0)
        m_serverUrl->addItem(url);
    m_serverUrl->setCurrentText(url);
    m_duration->setValue(settings.value(QStringLiteral("test/duration"), m_duration->value()).toInt());
    m_streams->setValue(settings.value(QStringLiteral("test/streams"), m_streams->value()).toInt());
    m_repeats->setValue(settings.value(QStringLiteral("test/repeats"), m_repeats->value()).toInt());
    m_pause->setValue(settings.value(QStringLiteral("test/pause"), m_pause->value()).toInt());
    m_contract->setValue(settings.value(QStringLiteral("load/contract"), m_contract->value()).toInt());
    m_loadDirection->setCurrentIndex(settings.value(QStringLiteral("load/upload"), false).toBool() ? 1 : 0);
    m_loadStages->setText(settings.value(QStringLiteral("load/stages"), m_loadStages->text()).toString());
    m_loadDuration->setValue(settings.value(QStringLiteral("load/duration"), m_loadDuration->value()).toInt());
    m_loadRuns->setValue(settings.value(QStringLiteral("load/runs"), m_loadRuns->value()).toInt());
    m_loadPause->setValue(settings.value(QStringLiteral("load/pause"), m_loadPause->value()).toInt());
    m_port->setValue(settings.value(QStringLiteral("server/port"), m_port->value()).toInt());
    m_ipVersion->setCurrentIndex(
        std::max(0, m_ipVersion->findData(settings.value(QStringLiteral("test/ipVersion"), 0).toInt())));
    for (const auto &[inputs, prefix] : {std::pair{&m_contractDown, QStringLiteral("contract/down")},
                                         std::pair{&m_contractUp, QStringLiteral("contract/up")}}) {
        inputs->max->setValue(settings.value(prefix + QStringLiteral("Max"), 0).toInt());
        inputs->normal->setValue(settings.value(prefix + QStringLiteral("Normal"), 0).toInt());
        inputs->min->setValue(settings.value(prefix + QStringLiteral("Min"), 0).toInt());
    }
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("test/server"), m_serverUrl->currentText().trimmed());
    settings.setValue(QStringLiteral("test/duration"), m_duration->value());
    settings.setValue(QStringLiteral("test/streams"), m_streams->value());
    settings.setValue(QStringLiteral("test/repeats"), m_repeats->value());
    settings.setValue(QStringLiteral("test/pause"), m_pause->value());
    settings.setValue(QStringLiteral("load/contract"), m_contract->value());
    settings.setValue(QStringLiteral("load/upload"), m_loadDirection->currentIndex() == 1);
    settings.setValue(QStringLiteral("load/stages"), m_loadStages->text());
    settings.setValue(QStringLiteral("load/duration"), m_loadDuration->value());
    settings.setValue(QStringLiteral("load/runs"), m_loadRuns->value());
    settings.setValue(QStringLiteral("load/pause"), m_loadPause->value());
    settings.setValue(QStringLiteral("server/port"), m_port->value());
    settings.setValue(QStringLiteral("test/ipVersion"), m_ipVersion->currentData().toInt());
    for (const auto &[inputs, prefix] : {std::pair{&m_contractDown, QStringLiteral("contract/down")},
                                         std::pair{&m_contractUp, QStringLiteral("contract/up")}}) {
        settings.setValue(prefix + QStringLiteral("Max"), inputs->max->value());
        settings.setValue(prefix + QStringLiteral("Normal"), inputs->normal->value());
        settings.setValue(prefix + QStringLiteral("Min"), inputs->min->value());
    }
}

// --- Shared -----------------------------------------------------------------

bool MainWindow::serverUrl(QUrl &url)
{
    const QString text = m_serverUrl->currentText().trimmed();
    url = QUrl::fromUserInput(text);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        QMessageBox::warning(this, windowTitle(), tr("Ungültige Server-Adresse: %1").arg(text));
        return false;
    }
    return true;
}

http::IpVersion MainWindow::ipVersion() const
{
    return http::IpVersion(m_ipVersion->currentData().toInt());
}

void MainWindow::showServerInfo(const http::ServerInfo &info)
{
    QStringList parts;
    if (!info.colo.isEmpty())
        parts << tr("Rechenzentrum %1").arg(info.colo);
    if (info.ipVersion())
        parts << tr("verbunden über IPv%1").arg(info.ipVersion());
    if (!info.clientIp.isEmpty())
        parts << tr("deine IP %1").arg(info.clientIp);
    if (!info.asn.isEmpty())
        parts << tr("AS%1").arg(info.asn);
    m_serverInfo->setText(parts.join(QStringLiteral(" · ")));
}

void MainWindow::loadHistory()
{
    QString error;
    const history::Data data = history::load(m_historyPath, &error);
    if (!error.isEmpty())
        m_status->setText(tr("Verlauf konnte nicht gelesen werden: %1").arg(error));

    m_speedRuns = data.speedRuns;
    m_series = data.lastSpeedSeries();
    for (const SpeedRun &run : std::as_const(m_speedRuns))
        addHistoryRow(run);
    updateSeriesAnalysis();

    m_lastLoadSeries = data.lastLoadSeries();
    if (!data.loadSeries.isEmpty()) {
        m_loadSeries = data.loadSeries.last();
        showLoadResults();
    }
}

void MainWindow::addHistoryRow(const SpeedRun &run)
{
    const SpeedTestResult &result = run.result;
    m_history->insertRow(0);
    m_history->setItem(0, 0, cell(QStringLiteral("%1.%2").arg(run.series).arg(run.index), Qt::AlignCenter));
    auto *time = cell(timeText(run.time), Qt::AlignLeft | Qt::AlignVCenter);
    time->setToolTip(QLocale().toString(run.time, QLocale::LongFormat));
    m_history->setItem(0, 1, time);
    m_history->setItem(0, 2, cell(serverText(run), Qt::AlignLeft | Qt::AlignVCenter));
    m_history->setItem(0, 3, cell(fmt::ms(result.pingMs)));
    m_history->setItem(0, 4, cell(fmt::ms(result.jitterMs)));
    m_history->setItem(0, 5, cell(fmt::mbps(result.downloadMbps)));
    m_history->setItem(0, 6, cell(fmt::mbps(result.uploadMbps)));
    m_history->setItem(0, 7, cell(loadedLatencyText(result)));
}

void MainWindow::showLoadResults()
{
    m_loadTable->setRowCount(0);
    for (qsizetype i = 0; i < m_loadSeries.runs.size(); ++i) {
        for (const LoadStageResult &stage : m_loadSeries.runs[i].stages)
            addLoadRow(m_loadTable, int(i + 1), stage, m_loadSeries.contractMbps);
    }
    QString summary = findingsHtml(report::loadFindings(m_loadSeries));
    if (!m_loadSeries.runs.isEmpty()) {
        summary = tr("<b>Letzter Lasttest vom %1</b><br>")
                      .arg(QLocale().toString(m_loadSeries.runs.first().time, QLocale::ShortFormat)) +
                  summary;
    }
    m_loadSummary->setText(summary);
}

void MainWindow::saveHistoryError(const QString &error)
{
    QMessageBox::warning(this, windowTitle(), tr("Die Messung konnte nicht im Verlauf gespeichert werden: %1\n%2")
                                                  .arg(error, QDir::toNativeSeparators(m_historyPath)));
}

void MainWindow::updateRunningState()
{
    // Only one measurement at a time; they would distort each other. A series
    // counts as running during the pauses between its runs, too.
    const bool speedRunning = m_test->isRunning() || m_speedSeriesActive;
    const bool loadRunning = m_load->isRunning() || m_loadSeriesActive;

    m_startButton->setText(speedRunning ? tr("Stopp") : tr("Test starten"));
    m_startButton->setEnabled(!loadRunning);
    for (QWidget *widget : std::initializer_list<QWidget *>{m_duration, m_streams, m_repeats, m_clearButton})
        widget->setEnabled(!speedRunning);
    m_pause->setEnabled(!speedRunning && m_repeats->value() > 1);

    m_loadButton->setText(loadRunning ? tr("Stopp") : tr("Test starten"));
    m_loadButton->setEnabled(!speedRunning);
    for (QWidget *widget :
         std::initializer_list<QWidget *>{m_contract, m_loadDirection, m_loadStages, m_loadDuration, m_loadRuns})
        widget->setEnabled(!loadRunning);
    m_loadPause->setEnabled(!loadRunning && m_loadRuns->value() > 1);

    const bool canExport = !speedRunning && !loadRunning && (!m_speedRuns.isEmpty() || !m_loadSeries.isEmpty());
    m_exportButton->setEnabled(canExport);
    m_loadExportButton->setEnabled(canExport);

    m_serverUrl->setEnabled(!speedRunning && !loadRunning);
    m_ipVersion->setEnabled(!speedRunning && !loadRunning);
    m_blockButton->setEnabled(!speedRunning && !loadRunning);
}

void MainWindow::startPause(int seconds, QLabel *status, std::function<void()> next)
{
    m_pauseNext = std::move(next);
    m_pauseStatus = status;
    m_pauseRemaining = seconds + 1;
    m_pauseTimer.start();
    // First tick right away, but outside the finished() handler that called
    // us: shows the countdown or, without a pause, starts the next run.
    QTimer::singleShot(0, this, [this] {
        if (m_pauseNext)
            pauseTick();
    });
}

void MainWindow::pauseTick()
{
    if (--m_pauseRemaining > 0) {
        m_pauseStatus->setText(tr("Pause · weiter in %1 s …").arg(m_pauseRemaining));
        return;
    }
    m_pauseTimer.stop();
    if (m_pauseNext)
        std::exchange(m_pauseNext, nullptr)();
}

void MainWindow::exportReport()
{
    QSettings settings;
    const QString directory =
        settings.value(QStringLiteral("export/directory"),
                       QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .toString();
    const QString name =
        QStringLiteral("broadband-%1.xlsx").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmm")));

    QString path = QFileDialog::getSaveFileName(this, tr("Messbericht exportieren"), QDir(directory).filePath(name),
                                                tr("Excel-Arbeitsmappe (*.xlsx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".xlsx"), Qt::CaseInsensitive))
        path += QLatin1String(".xlsx");
    settings.setValue(QStringLiteral("export/directory"), QFileInfo(path).absolutePath());

    QString error;
    if (!report::saveXlsx(path, m_speedRuns, m_loadSeries, &error)) {
        QMessageBox::warning(this, windowTitle(), tr("Export fehlgeschlagen: %1").arg(error));
        return;
    }
    const QString message = tr("Exportiert: %1").arg(QDir::toNativeSeparators(path));
    m_status->setText(message);
    m_loadStatus->setText(message);
}

// --- Speed test -------------------------------------------------------------

void MainWindow::toggleTest()
{
    if (m_test->isRunning() || m_speedSeriesActive) {
        m_speedSeriesActive = false;
        m_pauseTimer.stop();
        m_pauseNext = nullptr;
        m_test->stop();
        m_status->setText(tr("Abgebrochen"));
        updateRunningState();
        return;
    }

    QUrl url;
    if (!serverUrl(url))
        return;

    m_speedConfig = SpeedTestConfig();
    m_speedConfig.baseUrl = url;
    m_speedConfig.durationSec = m_duration->value();
    m_speedConfig.streams = m_streams->value();
    m_speedConfig.ipVersion = ipVersion();
    m_serverInfo->clear();

    ++m_series;
    m_seriesRun = 0;
    m_seriesTotal = m_repeats->value();
    m_speedSeriesActive = true;
    updateSeriesAnalysis();
    startNextSpeedRun();
}

void MainWindow::startNextSpeedRun()
{
    if (!m_speedSeriesActive)
        return;
    ++m_seriesRun;

    m_chart->reset(m_speedConfig.durationSec);
    for (Tile *tile : {&m_pingTile, &m_downloadTile, &m_uploadTile})
        tile->value->setText(QStringLiteral("–"));
    m_pingTile.detail->setText(tr("ms"));
    m_downloadTile.detail->setText(tr("Mbit/s"));
    m_uploadTile.detail->setText(tr("Mbit/s"));

    m_test->start(m_speedConfig);
}

QString MainWindow::seriesPrefix() const
{
    return m_seriesTotal > 1 ? tr("Messung %1/%2 · ").arg(m_seriesRun).arg(m_seriesTotal) : QString();
}

void MainWindow::onPhaseChanged(SpeedTest::Phase phase)
{
    updateRunningState();
    if (phase != SpeedTest::Phase::Idle && phase != SpeedTest::Phase::Done)
        m_status->setText(seriesPrefix() + phaseText(phase));
    if (phase == SpeedTest::Phase::Idle)
        m_progress->setValue(0);
    else if (phase == SpeedTest::Phase::Done)
        m_progress->setValue(m_progress->maximum());
}

void MainWindow::onProgress(double fraction)
{
    double overall = 0.0;
    switch (m_test->phase()) {
    case SpeedTest::Phase::Latency:
        overall = fraction * kLatencyWeight;
        break;
    case SpeedTest::Phase::Download:
        overall = kLatencyWeight + fraction * kTransferWeight;
        break;
    case SpeedTest::Phase::Upload:
        overall = kLatencyWeight + kTransferWeight + fraction * kTransferWeight;
        break;
    default:
        return;
    }
    m_progress->setValue(int(overall * m_progress->maximum()));
}

void MainWindow::onSample(SpeedTest::Phase phase, double seconds, double mbps)
{
    const bool download = phase == SpeedTest::Phase::Download;
    m_chart->addSample(download ? m_downloadSeries : m_uploadSeries, seconds, mbps);
    (download ? m_downloadTile : m_uploadTile).value->setText(fmt::mbps(mbps));
}

void MainWindow::onPhaseFinished(SpeedTest::Phase phase, double mbps)
{
    const bool download = phase == SpeedTest::Phase::Download;
    const qint64 bytes = download ? m_test->result().downloadedBytes : m_test->result().uploadedBytes;
    const double latency = download ? m_test->result().downloadLatencyMs : m_test->result().uploadLatencyMs;
    Tile &tile = download ? m_downloadTile : m_uploadTile;
    tile.value->setText(fmt::mbps(mbps));
    QString detail = tr("Mbit/s · %1 übertragen").arg(fmt::bytes(bytes));
    if (latency >= 0)
        detail += tr(" · Ping %1 ms").arg(fmt::ms(latency));
    tile.detail->setText(detail);
    tile.detail->setToolTip(tr("Ping während %1: steigt er stark an, puffert der Router zu viel (Bufferbloat).")
                                .arg(download ? tr("des Downloads") : tr("des Uploads")));
}

void MainWindow::onFinished(const SpeedTestResult &result)
{
    SpeedRun run;
    run.time = QDateTime::currentDateTime();
    run.server = m_speedConfig.baseUrl.toString();
    run.series = m_series;
    run.index = m_seriesRun;
    run.streams = m_speedConfig.streams;
    run.durationSec = m_speedConfig.durationSec;
    run.ipVersion = result.server.ipVersion();
    run.result = result;
    m_speedRuns.append(run);
    addHistoryRow(run);
    updateSeriesAnalysis();
    updateContractCheck();

    QString error;
    if (!history::append(m_historyPath, run, &error))
        saveHistoryError(error);

    QString text = tr("Fertig: %1 Mbit/s down, %2 Mbit/s up, %3 ms Ping")
                       .arg(fmt::mbps(result.downloadMbps), fmt::mbps(result.uploadMbps), fmt::ms(result.pingMs));
    if (result.retries > 0)
        text += tr(" · %n Verbindung(en) neu aufgebaut", nullptr, result.retries);
    m_status->setText(seriesPrefix() + text);

    if (m_speedSeriesActive && m_seriesRun < m_seriesTotal) {
        startPause(m_pause->value(), m_status, [this] { startNextSpeedRun(); });
    } else {
        m_speedSeriesActive = false;
        if (m_seriesTotal > 1)
            m_status->setText(tr("Serie mit %n Messungen abgeschlossen, Auswertung siehe unten.", nullptr, m_seriesRun));
    }
    updateRunningState();
}

void MainWindow::onFailed(const QString &message)
{
    m_speedSeriesActive = false;
    m_pauseTimer.stop();
    m_pauseNext = nullptr;
    m_status->setText(m_seriesTotal > 1 ? tr("Fehler bei Messung %1/%2, Serie abgebrochen")
                                              .arg(m_seriesRun)
                                              .arg(m_seriesTotal)
                                        : tr("Fehler"));
    updateRunningState();
    QMessageBox::warning(this, windowTitle(), message);
}

QList<SpeedRun> MainWindow::currentSeries() const
{
    QList<SpeedRun> runs;
    for (const SpeedRun &run : m_speedRuns) {
        if (run.series == m_series)
            runs.append(run);
    }
    return runs;
}

// Marks outliers of every series in the history and describes the current one.
void MainWindow::updateSeriesAnalysis()
{
    const int rows = m_history->rowCount();
    for (qsizetype i = 0; i < m_speedRuns.size(); ++i) {
        const SpeedRun &run = m_speedRuns[i];
        QList<SpeedRun> series;
        for (const SpeedRun &other : std::as_const(m_speedRuns)) {
            if (other.series == run.series)
                series.append(other);
        }
        const int row = rows - 1 - int(i); // newest on top
        const struct
        {
            report::Metric metric;
            int column;
        } columns[] = {{report::Metric::Ping, 3},
                       {report::Metric::Jitter, 4},
                       {report::Metric::Download, 5},
                       {report::Metric::Upload, 6}};
        for (const auto &c : columns) {
            QTableWidgetItem *item = m_history->item(row, c.column);
            if (!item)
                continue;
            const stats::Summary s = stats::summarize(report::metricValues(series, c.metric));
            const bool outlier = stats::isOutlier(report::metricValue(run, c.metric), s);
            item->setForeground(outlier ? QBrush(kBadColor) : QBrush());
            QFont font = item->font();
            font.setBold(outlier);
            item->setFont(font);
            item->setToolTip(outlier ? tr("Ausreißer: Median der Serie %1 %2")
                                           .arg(c.metric == report::Metric::Download || c.metric == report::Metric::Upload
                                                    ? fmt::mbps(s.median)
                                                    : fmt::ms(s.median),
                                                report::metricUnit(c.metric))
                                     : QString());
        }
    }

    const QList<SpeedRun> series = currentSeries();
    const QList<Finding> loaded = report::loadedLatencyFindings(series);
    if (series.size() < 2 && loaded.isEmpty()) {
        m_seriesAnalysis->clear();
        m_seriesAnalysis->hide();
        return;
    }
    const QList<Finding> findings = series.size() < 2 ? loaded : report::speedFindings(series) + loaded;
    m_seriesAnalysis->setText(tr("<b>Auswertung Serie %1</b><br>").arg(m_series) + findingsHtml(findings));
    m_seriesAnalysis->show();
}

void MainWindow::clearHistory()
{
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Alle %n gespeicherten Speedtests endgültig löschen? Die Lasttests bleiben erhalten.", nullptr,
           int(m_speedRuns.size())));
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    if (!history::removeSpeedRuns(m_historyPath, &error)) {
        QMessageBox::warning(this, windowTitle(), tr("Verlauf konnte nicht gelöscht werden: %1").arg(error));
        return;
    }
    m_speedRuns.clear();
    m_history->setRowCount(0);
    updateSeriesAnalysis();
    updateContractCheck();
    updateRunningState();
}

// --- Load test --------------------------------------------------------------

void MainWindow::toggleLoadTest()
{
    if (m_load->isRunning() || m_loadSeriesActive) {
        const bool running = m_load->isRunning();
        m_loadSeriesActive = false;
        m_pauseTimer.stop();
        m_pauseNext = nullptr;
        m_load->stop(); // emits stopped() while running
        if (!running)
            m_loadStatus->setText(tr("Abgebrochen"));
        updateRunningState();
        return;
    }

    QUrl url;
    if (!serverUrl(url))
        return;

    const QList<int> stages = parseStages(m_loadStages->text());
    if (stages.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Bitte die Stufen als Zahlen zwischen 1 und 64 angeben, z. B. „1, 2, 4, 8, 16“."));
        return;
    }

    m_loadConfig = LoadTestConfig();
    m_loadConfig.baseUrl = url;
    m_loadConfig.stages = stages;
    m_loadConfig.stageDurationSec = m_loadDuration->value();
    m_loadConfig.upload = m_loadDirection->currentIndex() == 1;
    m_loadConfig.ipVersion = ipVersion();

    m_loadSeries = LoadSeries();
    m_loadSeries.id = ++m_lastLoadSeries;
    m_loadSeries.server = url.toString();
    m_loadSeries.upload = m_loadConfig.upload;
    m_loadSeries.contractMbps = m_contract->value();
    m_loadSeries.stageDurationSec = m_loadConfig.stageDurationSec;
    m_loadRun = 0;
    m_loadRunTotal = m_loadRuns->value();
    m_loadSeriesActive = true;

    m_loadChart->reset(LoadTest::totalDuration(m_loadConfig) * m_loadRunTotal);
    m_loadTable->setRowCount(0);
    m_loadSummary->clear();
    m_loadProgress->setValue(0);
    startNextLoadRun();
}

void MainWindow::startNextLoadRun()
{
    if (!m_loadSeriesActive)
        return;
    ++m_loadRun;
    m_loadRunStart = QDateTime::currentDateTime();
    m_loadStatus->setText(
        (m_loadRunTotal > 1 ? tr("Durchlauf %1/%2 · ").arg(m_loadRun).arg(m_loadRunTotal) : QString()) +
        tr("Messe Ping ohne Last …"));
    m_load->start(m_loadConfig);
    updateRunningState();
}

void MainWindow::updateContractLine()
{
    const int contract = m_contract->value();
    m_loadChart->setReferenceLine(contract, tr("Vertrag %1 Mbit/s").arg(contract));
}

void MainWindow::onLoadStageFinished(int, const LoadStageResult &result)
{
    addLoadRow(m_loadTable, m_loadRun, result, m_loadSeries.contractMbps);
    m_loadTable->scrollToBottom();
}

void MainWindow::onLoadFinished()
{
    LoadRun run;
    run.time = m_loadRunStart;
    run.idleLatencyMs = m_load->idleLatencyMs();
    run.stages = m_load->results();
    m_loadSeries.runs.append(run);

    QString error;
    if (!history::append(m_historyPath, m_loadSeries, run, &error))
        saveHistoryError(error);

    QString summary = findingsHtml(report::loadFindings(m_loadSeries));
    if (m_loadRunTotal > 1) {
        summary = tr("<b>Auswertung nach %1 von %2 Durchläufen</b><br>").arg(m_loadRun).arg(m_loadRunTotal) + summary;
    }
    m_loadSummary->setText(summary);

    if (m_loadSeriesActive && m_loadRun < m_loadRunTotal) {
        startPause(m_loadPause->value(), m_loadStatus, [this] { startNextLoadRun(); });
    } else {
        m_loadSeriesActive = false;
        m_loadStatus->setText(tr("Fertig"));
    }
    updateRunningState();
}

// --- Contract check ---------------------------------------------------------

contract::Contract MainWindow::currentContract() const
{
    contract::Contract c;
    for (const auto &[speeds, inputs] : {std::pair{&c.download, &m_contractDown}, std::pair{&c.upload, &m_contractUp}}) {
        speeds->maxMbps = inputs->max->value();
        speeds->normalMbps = inputs->normal->value();
        speeds->minMbps = inputs->min->value();
    }
    return c;
}

void MainWindow::updateContractCheck()
{
    if (!m_contractDays)
        return;
    const contract::Contract c = currentContract();
    const contract::Result result = contract::evaluate(internetRuns(m_speedRuns), c);

    auto range = [](const QList<SpeedRun> &runs, report::Metric metric) {
        const stats::Summary s = stats::summarize(report::metricValues(runs, metric));
        return s.count ? QStringLiteral("%1 – %2").arg(fmt::mbps(s.min), fmt::mbps(s.max)) : QStringLiteral("–");
    };
    // Red if the minimum was undercut that day, green if 90 % of the maximum was reached.
    auto rated = [](QTableWidgetItem *item, const contract::Speeds &speeds, const QList<double> &values) {
        if (speeds.minMbps > 0 && std::any_of(values.begin(), values.end(), [&](double v) { return v < speeds.minMbps; }))
            item->setForeground(kBadColor);
        else if (speeds.maxMbps > 0 &&
                 std::any_of(values.begin(), values.end(), [&](double v) { return v >= speeds.maxMbps * kContractThreshold; }))
            item->setForeground(kGoodColor);
        return item;
    };

    m_contractDays->setRowCount(0);
    for (const contract::Day &day : result.days) {
        const int row = m_contractDays->rowCount();
        m_contractDays->insertRow(row);
        m_contractDays->setItem(row, 0, cell(QLocale().toString(day.date, QStringLiteral("ddd dd.MM.yyyy")),
                                             Qt::AlignLeft | Qt::AlignVCenter));
        m_contractDays->setItem(row, 1, cell(QStringLiteral("%1 / %2").arg(day.runs.size()).arg(contract::kPerDay)));
        m_contractDays->setItem(row, 2, rated(cell(range(day.runs, report::Metric::Download)), c.download,
                                              report::metricValues(day.runs, report::Metric::Download)));
        m_contractDays->setItem(row, 3, rated(cell(range(day.runs, report::Metric::Upload)), c.upload,
                                              report::metricValues(day.runs, report::Metric::Upload)));
        auto *protocol = cell(day.protocolIssues.isEmpty() ? tr("✔ passt") : day.protocolIssues.join(QStringLiteral("; ")),
                              Qt::AlignLeft | Qt::AlignVCenter);
        protocol->setForeground(day.protocolIssues.isEmpty() ? kGoodColor : kWarnColor);
        m_contractDays->setItem(row, 4, protocol);
    }

    QList<Finding> findings = result.findings;
    for (const QString &issue : result.protocolIssues)
        findings.insert(1, Finding{Finding::Warning, issue});
    if (result.days.isEmpty()) {
        findings = {Finding{Finding::Info, tr("Noch keine Speedtests gegen einen Internet-Server im Verlauf.")}};
    }
    m_contractSummary->setText(findingsHtml(findings));
}

void MainWindow::startMeasurementBlock()
{
    QUrl url;
    if (!serverUrl(url))
        return;
    if (http::proxyFor(url).type() == QNetworkProxy::NoProxy) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Für die Vertragsprüfung bitte einen Internet-Server wie %1 eintragen, "
                                    "keinen Rechner im LAN.")
                                     .arg(kDefaultServer));
        return;
    }
    m_repeats->setValue(5);
    m_pause->setValue(contract::kMinSpacingMin * 60);
    m_tabs->setCurrentIndex(0);
    toggleTest();
}

// --- LAN server -------------------------------------------------------------

void MainWindow::toggleServer()
{
    if (m_server->isListening()) {
        m_server->stop();
        m_serverStatsTimer.stop();
    } else if (m_server->start(quint16(m_port->value()))) {
        m_serverStatsTimer.start();
    } else {
        QMessageBox::warning(this, windowTitle(),
                             tr("Server konnte nicht gestartet werden: %1").arg(m_server->errorString()));
    }
    updateServerStatus();
}

void MainWindow::updateServerStatus()
{
    const bool listening = m_server->isListening();
    m_serverButton->setText(listening ? tr("Server stoppen") : tr("Server starten"));
    m_useLocalButton->setEnabled(listening);
    m_port->setEnabled(!listening);

    if (!listening) {
        m_serverState->setText(tr("Gestoppt"));
        m_serverAddresses->setText(QStringLiteral("–"));
    } else {
        m_serverState->setText(tr("Läuft auf Port %1 · %n Verbindung(en)", nullptr, m_server->connectionCount())
                                   .arg(m_server->port()));
        QStringList urls;
        const auto addresses = QNetworkInterface::allAddresses();
        for (const QHostAddress &address : addresses) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
                urls << QStringLiteral("http://%1:%2").arg(address.toString()).arg(m_server->port());
        }
        m_serverAddresses->setText(urls.isEmpty() ? QStringLiteral("http://localhost:%1").arg(m_server->port())
                                                  : urls.join(QLatin1Char('\n')));
    }
    m_serverTraffic->setText(tr("%1 gesendet · %2 empfangen")
                                 .arg(fmt::bytes(m_server->bytesSent()), fmt::bytes(m_server->bytesReceived())));
}
