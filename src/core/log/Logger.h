#pragma once

#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string_view>

namespace blendnet {

enum class LogLevel { Debug, Info, Warning, Error, Off };
enum class LogError { None, OpenFailed, WriteFailed };

std::string_view toString(LogLevel level);
std::optional<LogLevel> parseLogLevel(std::string_view value);

// RNF-07 : sorties sérialisées entre threads, horodatage UTC et niveau réglable.
// Ne jamais passer de token ni de configuration complète au journal.
class Logger {
  public:
    explicit Logger(std::ostream& output, LogLevel level = LogLevel::Info);

    void setLevel(LogLevel level);
    LogLevel level() const;
    LogError openFile(const std::filesystem::path& path);
    LogError log(LogLevel level, std::string_view message);

  private:
    mutable std::mutex m_mutex;
    std::ostream& m_output;
    std::ofstream m_file;
    LogLevel m_level;
};

} // namespace blendnet
