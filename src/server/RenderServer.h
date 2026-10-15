#pragma once
// Rôle source. Squelette P1 : sera rempli en P3 (TLS, sessions) et P5 (jobs).
#include <QObject>

namespace blendnet {
class RenderServer : public QObject {
    Q_OBJECT
public:
    explicit RenderServer(QObject* parent = nullptr);
};
} // namespace blendnet
