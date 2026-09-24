#include "MainWindow.h"
#include "TestServer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QNetworkProxyFactory>
#include <QTextStream>

#include <cstdlib>
#include <cstring>

namespace {

struct Options
{
    bool server = false;
    quint16 port = 8080;
    QString url;
};

Options parseArguments(const QCoreApplication &app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Einfacher Bandbreiten- und Internet-Speedtest."));
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
        QCoreApplication::translate("main", "Server-Adresse, die in der GUI vorausgewählt wird."),
        QStringLiteral("url"));
    parser.addOptions({serverOption, portOption, urlOption});
    parser.process(app);

    Options options;
    options.server = parser.isSet(serverOption);
    options.url = parser.value(urlOption);

    bool ok = false;
    const uint port = parser.value(portOption).toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        QTextStream(stderr) << QCoreApplication::translate("main", "Ungültiger Port: %1\n")
                                   .arg(parser.value(portOption));
        std::exit(2);
    }
    options.port = quint16(port);
    return options;
}

// Server mode and --help/--version need no display, so decide before a
// QApplication is created.
bool isHeadless(int argc, char *argv[])
{
    static const char *const headlessArgs[] = {"-s", "--server", "-h", "--help", "--help-all", "-?",
                                               "-v", "--version"};
    for (int i = 1; i < argc; ++i) {
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
        return runServer(app, parseArguments(app)); // --help/--version exit while parsing
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
