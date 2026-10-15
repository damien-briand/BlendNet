#pragma once
// RF-10 (sources connues), RNF-S2/S3 (token, empreinte épinglée), RF-24 (dossiers).
#include <QList>
#include <QSettings>
#include <QString>
#include <memory>

#include "log/Logger.h"

namespace blendnet {

struct KnownSource {
    QString host;
    quint16 port = 0;
    QString token;               // code d'appairage (jamais loggé)
    QString pinnedFingerprint;   // SHA-256 du certificat serveur, vide avant 1er appairage

    bool operator==(const KnownSource&) const = default;
};

class AppSettings {
public:
    static constexpr quint16 kDefaultPort = 7878;

    // Config standard de l'application (QSettings natif).
    AppSettings();
    // Config dans un fichier INI précis (tests, mode portable).
    explicit AppSettings(const QString& iniPath);

    QList<KnownSource> knownSources() const;
    void setKnownSources(const QList<KnownSource>& sources);

    quint16 listenPort() const;
    void setListenPort(quint16 port);

    QString workDir() const;
    void setWorkDir(const QString& dir);

    LogLevel logLevel() const;
    void setLogLevel(LogLevel level);

    void sync() { m_settings->sync(); }

private:
    std::unique_ptr<QSettings> m_settings;
};

} // namespace blendnet
