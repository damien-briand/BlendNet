#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace blendnet::test {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic<unsigned long long> sequence{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("blendnet-test-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(sequence++));
        if (!std::filesystem::create_directory(m_path)) {
            throw std::runtime_error("Cannot create test directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
};

inline void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary);
    output << content;
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot write test fixture");
    }
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read test fixture");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace blendnet::test
