#include "AppController.h"

namespace blendnet {
QString AppController::version() const { return QStringLiteral(BLENDNET_VERSION); }
} // namespace blendnet
