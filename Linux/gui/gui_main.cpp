#include "gui_controller.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QVariant>

namespace {

const QString kInstanceServerName = QStringLiteral("opendisplay-gui");

bool showRunningInstance() {
    QLocalSocket socket;
    socket.connectToServer(kInstanceServerName);
    if (!socket.waitForConnected(1000)) return false;
    socket.write("show\n");
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(1000);
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("OpenDisplay"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("OpenDisplay"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.opendisplay.desktop"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/icon-256.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Use an iOS device as a Wayland display."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption activateExistingOption(
        QStringLiteral("activate-existing"),
        QStringLiteral("If OpenDisplay is already running, show its configuration "
                       "window instead of starting a second instance."));
    parser.addOption(activateExistingOption);
    parser.process(application);

    if (parser.isSet(activateExistingOption) && showRunningInstance()) return 0;

    od::gui::GuiController controller;

    // Accept show requests from later launches started with --activate-existing.
    QLocalServer instanceServer;
    if (!instanceServer.listen(kInstanceServerName)) {
        QLocalServer::removeServer(kInstanceServerName);
        instanceServer.listen(kInstanceServerName);
    }
    QObject::connect(&instanceServer, &QLocalServer::newConnection, &controller,
                     [&instanceServer, &controller]() {
                         while (QLocalSocket* connection
                                = instanceServer.nextPendingConnection()) {
                             connection->close();
                             connection->deleteLater();
                         }
                         controller.showWindow();
                     });

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    });
    engine.loadFromModule(QStringLiteral("org.opendisplay.desktop"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    return application.exec();
}
