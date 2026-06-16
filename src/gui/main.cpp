#include "model.hpp"
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
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
        // Borrowed QML objects; smoke verifies that the commands are present and
        // disabled before a transfer, in addition to loading the QML module.
        for (const auto* name : {"pauseButton", "continueButton"}) {
            const auto* button = window->findChild<QObject*>(QString::fromLatin1(name));
            if (button == nullptr || button->property("enabled").toBool())
                return 1;
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
            if (!model.transfer_supported()) {
                if (area->isEnabled())
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
            if (!drop.isAccepted() || model.selected_source() != url.toLocalFile())
                application.exit(1);
        });
        QTimer::singleShot(200, &application, [window, &application] {
            window->close();
            window->releaseResources();
            QTimer::singleShot(50, &application, &QGuiApplication::quit);
        });
    }
    return application.exec();
}
