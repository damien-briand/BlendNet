#pragma once

#include <charconv>
#include <csignal>
#include <optional>
#include <string_view>

namespace blendnet::cli {
inline volatile std::sig_atomic_t interrupted = 0;
inline void onSignal(int) { interrupted = 1; }
inline void handleSignals() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
}
inline std::optional<unsigned int> number(std::string_view text, unsigned int maximum) {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > maximum)
        return std::nullopt;
    return value;
}
} // namespace blendnet::cli
