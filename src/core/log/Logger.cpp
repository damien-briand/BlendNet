#include "log/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace blendnet {

std::string_view toString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    case LogLevel::Off:
        return "off";
    }
    return "off";
}

std::optional<LogLevel> parseLogLevel(std::string_view value) {
    for (const auto level :
         {LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error, LogLevel::Off}) {
        if (toString(level) == value) {
            return level;
        }
    }
    return std::nullopt;
}

Logger::Logger(std::ostream& output, LogLevel level) : m_output(output), m_level(level) {}

void Logger::setLevel(LogLevel level) {
    const std::lock_guard lock(m_mutex);
    m_level = level;
}

LogLevel Logger::level() const {
    const std::lock_guard lock(m_mutex);
    return m_level;
}

LogError Logger::openFile(const std::filesystem::path& path) {
    const std::lock_guard lock(m_mutex);
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return LogError::OpenFailed;
        }
    }
    std::ofstream file(path, std::ios::app);
    if (!file) {
        return LogError::OpenFailed;
    }
    m_file = std::move(file);
    return LogError::None;
}

LogError Logger::log(LogLevel level, std::string_view message) {
    const std::lock_guard lock(m_mutex);
    if (level == LogLevel::Off || level < m_level) {
        return LogError::None;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream line;
    line << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << " [" << toString(level) << "] ";
    // Une entrée reste sur une ligne, même avec un message provenant d'un sous-processus.
    for (const char character : message) {
        switch (character) {
        case '\n':
            line << "\\n";
            break;
        case '\r':
            line << "\\r";
            break;
        default:
            line << character;
            break;
        }
    }
    line << '\n';
    const auto text = line.str();
    m_output << text << std::flush;
    if (m_file.is_open()) {
        m_file << text << std::flush;
    }
    return (!m_output || (m_file.is_open() && !m_file)) ? LogError::WriteFailed : LogError::None;
}

} // namespace blendnet
