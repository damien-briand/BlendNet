#pragma once
// Contrôleur racine de démonstration (P1). Les vrais contrôleurs arrivent en P8.
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace blendnet {
class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    explicit AppController(QObject* parent = nullptr) : QObject(parent) {}
    QString appName() const { return QStringLiteral("BlendNet"); }
    QString version() const;
};
} // namespace blendnet
