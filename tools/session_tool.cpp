#include "bridge/app/session.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication runtime(argc, argv);
    const auto args = runtime.arguments();
    if (args.size() != 6 || (args[1] != "receive" && args[1] != "connect"))
        return 64;
    bool valid = false;
    const auto number = args[3].toUInt(&valid);
    if (!valid || number > 65535 || args[4] != "--commands")
        return 64;
    bridge::Logger logger(std::cerr);
    bridge::app::Session session(
        args[1] == "receive" ? bridge::Role::receiver : bridge::Role::initiator, logger);
    QTextStream output(stdout);
    QObject::connect(&session, &bridge::app::Session::pairing_pending, &runtime,
                     [&](const QString& fingerprint) {
                         output << "{\"event\":\"fingerprint\",\"value\":\"" << fingerprint << "\"}"
                                << Qt::endl;
                     });
    QObject::connect(&session, &bridge::app::Session::finished, &runtime, [&](bool ok, int code) {
        output << "{\"event\":\"" << (ok ? "complete" : "failed") << "\",\"code\":" << code << "}"
               << Qt::endl;
        runtime.exit(ok ? 0 : 2);
    });
    QTimer commands;
    QByteArray previous;
    QObject::connect(&commands, &QTimer::timeout, &runtime, [&] {
        QFile input(args[5]);
        if (!input.open(QIODevice::ReadOnly))
            return;
        if (input.size() > 32) {
            session.cancel();
            return;
        }
        const auto command = input.read(32).trimmed();
        if (command == previous)
            return;
        previous = command;
        if (command == "confirm")
            session.confirm();
        else if (command == "reject")
            session.reject();
        else if (command == "cancel")
            session.cancel();
    });
    commands.start(10);
    if (args[1] == "receive") {
        auto port = session.receive(QHostAddress(args[2]), static_cast<std::uint16_t>(number));
        if (!port)
            return 2;
        output << "{\"event\":\"ready\",\"port\":" << *port << "}" << Qt::endl;
    } else {
        if (!session.connect_peer(QHostAddress(args[2]), static_cast<std::uint16_t>(number)))
            return 2;
    }
    return runtime.exec();
}
