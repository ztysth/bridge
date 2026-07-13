#include "model.hpp"
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
int main(int argc, char** argv) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("bridge"));
    SessionModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("sessionModel"), &model);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Bridge/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto* window =
        qobject_cast<QQuickWindow*>(engine.rootObjects().front()); // Borrowed from engine.
    if (window == nullptr)
        return 1;
    window->setPersistentSceneGraph(false);
    window->setPersistentGraphics(false);
    std::optional<QTemporaryDir> drop_fixture;
    if (application.arguments().contains(QStringLiteral("--smoke-test"))) {
        if (QQuickStyle::name() != QStringLiteral("Basic"))
            return 1;
        window->setProperty("smokeTest", true);
        // Borrowed QML objects; smoke verifies that the commands are present and
        // disabled before a transfer, in addition to loading the QML module.
        for (const auto* name : {"pauseButton", "continueButton"}) {
            const auto* button = window->findChild<QObject*>(QString::fromLatin1(name));
            if (button == nullptr || button->property("enabled").toBool()) {
                std::cerr << "GUI smoke: initial pause controls invalid\n";
                return 1;
            }
        }
        // Exercise real QQuickWindow drag events and QML -> QList<QUrl> conversion.
        drop_fixture.emplace();
        if (!drop_fixture->isValid())
            return 1;
        const auto url = QUrl::fromLocalFile(drop_fixture->path());
        QTimer::singleShot(50, &application, [window, &application, &model, url] {
            auto* area = window->findChild<QQuickItem*>(QStringLiteral("sourceDropArea"));
            if (!area) {
                application.exit(1);
                return;
            }
            if (!area->isEnabled()) {
                application.exit(1);
                return;
            }
            const auto point = area->mapToScene(QPointF(area->width() / 2, area->height() / 2));
            QMimeData mime;
            mime.setUrls({url});
            QDragEnterEvent enter(point.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                                  Qt::NoModifier);
            QCoreApplication::sendEvent(window, &enter);
            QDragMoveEvent move(point.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(window, &move);
            QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &drop);
            if (!drop.isAccepted() || model.selected_source() != url.toLocalFile()) {
                std::cerr << "GUI smoke: drop failed\n";
                application.exit(1);
                return;
            }
            // Click the actual QML buttons; forcing fallback dialogs in this
            // diagnostic mode makes their visibility observable in automation.
            for (const auto& names : {std::pair{"chooseFileButton", "filePicker"},
                                      std::pair{"chooseFolderButton", "folderPicker"},
                                      std::pair{"chooseDestinationButton", "destinationPicker"}}) {
                window->setProperty("receiving", QString::fromLatin1(names.first) ==
                                                     QStringLiteral("chooseDestinationButton"));
                QCoreApplication::processEvents();
                auto* button = window->findChild<QQuickItem*>(QString::fromLatin1(names.first));
                auto* dialog = window->findChild<QObject*>(QString::fromLatin1(names.second));
                if (!button || !button->isEnabled() || !button->isVisible() || !dialog) {
                    qCritical("Picker smoke: button/dialog missing or unavailable: %s",
                              names.first);
                    application.exit(1);
                    return;
                }
                const auto local =
                    button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
                const QPointF global(window->mapToGlobal(local.toPoint()));
                QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                press.setTimestamp(100);
                QCoreApplication::sendEvent(window, &press);
                // Construct after press delivery so Qt's pointer-device grab
                // state is present in the release event.
                QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                release.setTimestamp(101);
                QCoreApplication::sendEvent(window, &release);
                QCoreApplication::processEvents();
                if (!dialog->property("visible").toBool() ||
                    !QMetaObject::invokeMethod(dialog, "close")) {
                    std::cerr << "Picker smoke: click failed for " << names.first << '\n';
                    application.exit(1);
                    return;
                }
                QCoreApplication::processEvents();
            }
            window->close();
            window->releaseResources();
            QTimer::singleShot(50, &application, &QGuiApplication::quit);
        });
    }
    return application.exec();
}
