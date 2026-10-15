// Client simulé headless (tests réseau, démos). Squelette P1 : implémenté en P3.
#include <QCoreApplication>

#include "log/Logger.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    BN_INFO("cli", QStringLiteral("BlendNet-cli : squelette (P1), connexion à implémenter en P3"));
    return 0;
}
