#pragma once
// Rôle client. Squelette P1 : sera rempli en P3 (connexion persistante, backoff).
#include <QObject>

namespace blendnet {
class SourceConnection : public QObject {
    Q_OBJECT
public:
    explicit SourceConnection(QObject* parent = nullptr);
};
} // namespace blendnet
