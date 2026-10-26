#pragma once
#include <span>
#include <string_view>
namespace blendnet::ui {
struct Translation {
    std::string_view key, fr, en;
};
std::span<const Translation> catalogue();
std::string_view translate(std::string_view key, std::string_view language);
} // namespace blendnet::ui
