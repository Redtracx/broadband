#include "Cli.h"
#include "MainWindow.h"
#include "TestServer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QTextStream>

#include <cstdlib>
#include <cstring>

namespace {

struct Options
{
    bool server = false;
    bool run = false; // speed test or load test on the command line
    quint16 port = 8080;
    QString url;
    CliOptions cli;
};

[[noreturn]] void usageError(const QString &message)
{
    QTextStream(stderr) << message << '\n';
    std::exit(2);
}

int intValue(const QCommandLineParser &parser, const QCommandLineOption &option, int min, int max)
{
    bool ok = false;
    const int value = parser.value(option).toInt(&ok);
    if (!ok || value < min || value > max) {
        usageError(QCoreApplication::translate("main", "Ungültiger Wert für --%1: %2 (erlaubt %3–%4)")
                       .arg(option.names().constLast(), parser.value(option))
                       .arg(min)
                       .arg(max));
    }
    return value;
}

Options parseArguments(const QCoreApplication &app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Einfacher Bandbreiten- und Internet-Speedtest.\n\n"
                                            "Ohne Optionen startet die GUI. Mit --run oder --load läuft die Messung "
                                            "auf der Kommandozeile, z. B. per cron; das Ergebnis wird im Verlauf "
                                            "gespeichert und erscheint auch in der GUI."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption serverOption(
        {QStringLiteral("s"), QStringLiteral("server")},
        QCoreApplication::translate("main", "Nur den LAN-Testserver starten (ohne GUI)."));
    const QCommandLineOption portOption(
        {QStringLiteral("p"), QStringLiteral("port")},
        QCoreApplication::translate("main", "Port des Testservers (Standard: 8080)."),
        QStringLiteral("port"), QStringLiteral("8080"));
    const QCommandLineOption urlOption(
        {QStringLiteral("u"), QStringLiteral("url")},
        QCoreApplication::translate("main", "Server-Adresse für die Messung bzw. die in der GUI vorausgewählte."),
        QStringLiteral("url"));
    const QCommandLineOption runOption(
        {QStringLiteral("r"), QStringLiteral("run")},
        QCoreApplication::translate("main", "Speedtest ohne GUI ausführen und das Ergebnis ausgeben."));
    const QCommandLineOption loadOption(
        {QStringLiteral("l"), QStringLiteral("load")},
        QCoreApplication::translate("main", "Lasttest ohne GUI mit diesen Stufen gleichzeitiger Clients, "
                                            "z. B. 1,2,4,8."),
        QStringLiteral("stufen"));
    const QCommandLineOption durationOption(
        {QStringLiteral("d"), QStringLiteral("duration")},
        QCoreApplication::translate("main", "Messdauer in Sekunden je Richtung bzw. je Stufe (Standard: 10)."),
        QStringLiteral("s"), QStringLiteral("10"));
    const QCommandLineOption streamsOption(
        {QStringLiteral("n"), QStringLiteral("streams")},
        QCoreApplication::translate("main", "Parallele Verbindungen beim Speedtest (Standard: 4)."),
        QStringLiteral("n"), QStringLiteral("4"));
    const QCommandLineOption uploadOption(
        QStringLiteral("upload"), QCoreApplication::translate("main", "Lasttest in Upload-Richtung."));
    const QCommandLineOption contractOption(
        QStringLiteral("contract"),
        QCoreApplication::translate("main", "Vertragliche Bandbreite in Mbit/s für die Auswertung des Lasttests."),
        QStringLiteral("mbit"), QStringLiteral("0"));
    const QCommandLineOption ipOption(
        QStringLiteral("ip"), QCoreApplication::translate("main", "Nur IPv4 (4) oder nur IPv6 (6) verwenden."),
        QStringLiteral("4|6"));
    const QCommandLineOption formatOption(
        {QStringLiteral("f"), QStringLiteral("format")},
        QCoreApplication::translate("main", "Ausgabe als text, json oder csv (Standard: text)."),
        QStringLiteral("format"), QStringLiteral("text"));
    const QCommandLineOption noHeaderOption(
        QStringLiteral("no-header"), QCoreApplication::translate("main", "Bei CSV keine Kopfzeile ausgeben."));
    const QCommandLineOption noSaveOption(
        QStringLiteral("no-save"), QCoreApplication::translate("main", "Ergebnis nicht im Verlauf speichern."));
    parser.addOptions({serverOption, portOption, urlOption, runOption, loadOption, durationOption, streamsOption,
                       uploadOption, contractOption, ipOption, formatOption, noHeaderOption, noSaveOption});
    parser.process(app);

    Options options;
    options.server = parser.isSet(serverOption);
    options.url = parser.value(urlOption);
    options.port = quint16(intValue(parser, portOption, 1, 65535));

    options.run = parser.isSet(runOption) || parser.isSet(loadOption);
    if (!options.run)
        return options;

    CliOptions &cli = options.cli;
    if (!options.url.isEmpty()) {
        cli.url = QUrl::fromUserInput(options.url);
        if (!cli.url.isValid() ||
            (cli.url.scheme() != QLatin1String("http") && cli.url.scheme() != QLatin1String("https")))
            usageError(QCoreApplication::translate("main", "Ungültige Server-Adresse: %1").arg(options.url));
    }
    if (parser.isSet(loadOption)) {
        const QStringList parts = parser.value(loadOption).split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                                                 Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            bool ok = false;
            const int clients = part.toInt(&ok);
            if (!ok || clients < 1 || clients > 64)
                usageError(QCoreApplication::translate("main", "Ungültige Stufen: %1 (Zahlen von 1 bis 64)")
                               .arg(parser.value(loadOption)));
            cli.loadStages << clients;
        }
        if (cli.loadStages.isEmpty())
            usageError(QCoreApplication::translate("main", "--load braucht mindestens eine Stufe, z. B. 1,4,16"));
    }
    cli.durationSec = intValue(parser, durationOption, cli.loadStages.isEmpty() ? 1 : 2, 3600);
    cli.streams = intValue(parser, streamsOption, 1, SpeedTest::kMaxStreams);
    cli.upload = parser.isSet(uploadOption);
    cli.contractMbps = intValue(parser, contractOption, 0, 100'000);

    if (parser.isSet(ipOption)) {
        const QString ip = parser.value(ipOption);
        if (ip == QLatin1String("4"))
            cli.ipVersion = http::IpVersion::V4;
        else if (ip == QLatin1String("6"))
            cli.ipVersion = http::IpVersion::V6;
        else
            usageError(QCoreApplication::translate("main", "--ip erwartet 4 oder 6"));
    }

    const QString format = parser.value(formatOption).toLower();
    if (format == QLatin1String("text"))
        cli.format = CliOptions::Format::Text;
    else if (format == QLatin1String("json"))
        cli.format = CliOptions::Format::Json;
    else if (format == QLatin1String("csv"))
        cli.format = CliOptions::Format::Csv;
    else
        usageError(QCoreApplication::translate("main", "Unbekanntes Format: %1 (text, json oder csv)").arg(format));
    cli.header = !parser.isSet(noHeaderOption);
    cli.save = !parser.isSet(noSaveOption);
    return options;
}

// Server mode, command line measurements and --help/--version need no
// display, so decide before a QApplication is created.
bool isHeadless(int argc, char *argv[])
{
    static const char *const headlessArgs[] = {"-s", "--server", "-r", "--run", "-l", "--load", "-h", "--help",
                                               "--help-all", "-?", "-v", "--version"};
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--load=", 7) == 0)
            return true;
        for (const char *arg : headlessArgs) {
            if (std::strcmp(argv[i], arg) == 0)
                return true;
        }
    }
    return false;
}

int runServer(QCoreApplication &app, const Options &options)
{
    TestServer server;
    QTextStream out(stdout);
    if (!server.start(options.port)) {
        QTextStream(stderr) << QCoreApplication::translate("main", "Server konnte nicht gestartet werden: %1\n")
                                   .arg(server.errorString());
        return 1;
    }
    out << QCoreApplication::translate("main", "Testserver läuft auf Port %1. Beenden mit Strg+C.\n")
               .arg(server.port());
    out.flush();
    return app.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("broadband"));
    QCoreApplication::setApplicationName(QStringLiteral("broadband"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BROADBAND_VERSION));

    // Honor http_proxy/https_proxy/no_proxy and the OS proxy settings.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    if (isHeadless(argc, argv)) {
        QCoreApplication app(argc, argv);
        const Options options = parseArguments(app); // --help/--version exit while parsing
        if (options.server)
            return runServer(app, options);
        return runCli(app, options.cli);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Broadband"));
    const Options options = parseArguments(app);

    MainWindow window;
    if (!options.url.isEmpty())
        window.setServerUrl(options.url);
    window.show();
    return app.exec();
}
