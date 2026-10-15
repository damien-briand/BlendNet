#pragma once
// RNF-07 : logs horodatés, niveau réglable. Règle §8 : le token ne doit JAMAIS apparaître.
#include <QMutex>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace blendnet {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

class Logger {
public:
    using Sink = std::function<void(const QString& formattedLine)>;

    static Logger& instance();

    void setLevel(LogLevel level) { m_level = level; }
    LogLevel level() const { return m_level; }

    // Sink personnalisé (tests, UI). Sans sink : stderr.
    void setSink(Sink sink);
    // Écriture additionnelle dans un fichier (export support). Chaîne vide = désactivé.
    void setFile(const QString& path);

    // Toute occurrence de `secret` sera remplacée par "***" dans les logs.
    void addSecret(const QString& secret);
    void clearSecrets();

    void log(LogLevel level, const QString& category, const QString& message);

    static const char* levelName(LogLevel level);
    static LogLevel levelFromString(const QString& name, LogLevel fallback = LogLevel::Info);

private:
    Logger() = default;
    QString redact(QString text) const;

    std::atomic<LogLevel> m_level{LogLevel::Info};
    mutable QMutex m_mutex;
    Sink m_sink;
    QString m_filePath;
    QStringList m_secrets;
};

} // namespace blendnet

#define BN_LOG(level, category, msg) ::blendnet::Logger::instance().log(level, category, msg)
#define BN_TRACE(cat, msg) BN_LOG(::blendnet::LogLevel::Trace, cat, msg)
#define BN_DEBUG(cat, msg) BN_LOG(::blendnet::LogLevel::Debug, cat, msg)
#define BN_INFO(cat, msg) BN_LOG(::blendnet::LogLevel::Info, cat, msg)
#define BN_WARN(cat, msg) BN_LOG(::blendnet::LogLevel::Warn, cat, msg)
#define BN_ERROR(cat, msg) BN_LOG(::blendnet::LogLevel::Error, cat, msg)
