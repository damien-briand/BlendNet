#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "config/AppSettings.h"
#include "log/Logger.h"

int main(int argc, char* argv[]) {
    // QGuiApplication (et non QApplication) : Qt Widgets est interdit (spec §2).
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("BlendNet"));
    QGuiApplication::setApplicationName(QStringLiteral("BlendNet"));

    blendnet::AppSettings settings;
    blendnet::Logger::instance().setLevel(settings.logLevel());
    BN_INFO("main", QStringLiteral("BlendNet démarre"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("BlendNet.Ui", "Main");
    return app.exec();
}
