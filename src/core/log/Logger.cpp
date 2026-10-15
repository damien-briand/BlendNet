#include "Logger.h"

#include <QDateTime>
#include <QFile>
#include <QMutexLocker>
#include <QTextStream>
#include <cstdio>

namespace blendnet {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setSink(Sink sink) {
    QMutexLocker lock(&m_mutex);
    m_sink = std::move(sink);
}

void Logger::setFile(const QString& path) {
    QMutexLocker lock(&m_mutex);
    m_filePath = path;
}

void Logger::addSecret(const QString& secret) {
    if (secret.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    if (!m_secrets.contains(secret)) m_secrets.append(secret);
}

void Logger::clearSecrets() {
    QMutexLocker lock(&m_mutex);
    m_secrets.clear();
}

QString Logger::redact(QString text) const {
    for (const QString& s : m_secrets) text.replace(s, QStringLiteral("***"));
    return text;
}

void Logger::log(LogLevel level, const QString& category, const QString& message) {
    if (level < m_level.load() || level == LogLevel::Off) return;

    QMutexLocker lock(&m_mutex);
    const QString line = QStringLiteral("%1 [%2] %3: %4")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                  QString::fromLatin1(levelName(level)), category, redact(message));

    if (m_sink) {
        m_sink(line);
    } else {
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
    }
    if (!m_filePath.isEmpty()) {
        QFile f(m_filePath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream(&f) << line << '\n';
        }
    }
}

const char* Logger::levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off: return "OFF";
    }
    return "?";
}

LogLevel Logger::levelFromString(const QString& name, LogLevel fallback) {
    const QString n = name.trimmed().toUpper();
    if (n == "TRACE") return LogLevel::Trace;
    if (n == "DEBUG") return LogLevel::Debug;
    if (n == "INFO") return LogLevel::Info;
    if (n == "WARN") return LogLevel::Warn;
    if (n == "ERROR") return LogLevel::Error;
    if (n == "OFF") return LogLevel::Off;
    return fallback;
}

} // namespace blendnet
