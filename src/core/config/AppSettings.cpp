#include "AppSettings.h"

#include <QStandardPaths>

namespace blendnet {

AppSettings::AppSettings()
    : m_settings(std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                             QStringLiteral("BlendNet"), QStringLiteral("BlendNet"))) {}

AppSettings::AppSettings(const QString& iniPath)
    : m_settings(std::make_unique<QSettings>(iniPath, QSettings::IniFormat)) {}

QList<KnownSource> AppSettings::knownSources() const {
    QList<KnownSource> out;
    const int n = m_settings->beginReadArray(QStringLiteral("sources"));
    for (int i = 0; i < n; ++i) {
        m_settings->setArrayIndex(i);
        KnownSource s;
        s.host = m_settings->value(QStringLiteral("host")).toString();
        s.port = static_cast<quint16>(m_settings->value(QStringLiteral("port"), kDefaultPort).toUInt());
        s.token = m_settings->value(QStringLiteral("token")).toString();
        s.pinnedFingerprint = m_settings->value(QStringLiteral("fingerprint")).toString();
        out.append(s);
    }
    m_settings->endArray();
    return out;
}

void AppSettings::setKnownSources(const QList<KnownSource>& sources) {
    m_settings->remove(QStringLiteral("sources"));
    m_settings->beginWriteArray(QStringLiteral("sources"), static_cast<int>(sources.size()));
    for (int i = 0; i < sources.size(); ++i) {
        m_settings->setArrayIndex(i);
        m_settings->setValue(QStringLiteral("host"), sources[i].host);
        m_settings->setValue(QStringLiteral("port"), sources[i].port);
        m_settings->setValue(QStringLiteral("token"), sources[i].token);
        m_settings->setValue(QStringLiteral("fingerprint"), sources[i].pinnedFingerprint);
    }
    m_settings->endArray();
}

quint16 AppSettings::listenPort() const {
    return static_cast<quint16>(m_settings->value(QStringLiteral("server/port"), kDefaultPort).toUInt());
}
void AppSettings::setListenPort(quint16 port) { m_settings->setValue(QStringLiteral("server/port"), port); }

QString AppSettings::workDir() const {
    const QString def = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/jobs";
    return m_settings->value(QStringLiteral("paths/workDir"), def).toString();
}
void AppSettings::setWorkDir(const QString& dir) { m_settings->setValue(QStringLiteral("paths/workDir"), dir); }

LogLevel AppSettings::logLevel() const {
    return Logger::levelFromString(m_settings->value(QStringLiteral("log/level"), "INFO").toString());
}
void AppSettings::setLogLevel(LogLevel level) {
    m_settings->setValue(QStringLiteral("log/level"), QString::fromLatin1(Logger::levelName(level)));
}

} // namespace blendnet
