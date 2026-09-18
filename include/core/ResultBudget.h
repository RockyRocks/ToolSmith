#pragma once
#include <string>
#include <string_view>
#include <cstddef>

/// Hard cap for any tool result text (UTF-8 bytes).
inline constexpr std::size_t kMaxResultBytes = 32768;

inline std::string CapHead(std::string text, bool& truncated) {
    truncated = false;
    if (text.size() <= kMaxResultBytes) return text;
    truncated = true;
    text.resize(kMaxResultBytes);
    return text;
}

inline std::string CapTail(std::string_view text, bool& truncated) {
    truncated = false;
    if (text.size() <= kMaxResultBytes) return std::string(text);
    truncated = true;
    return std::string(text.substr(text.size() - kMaxResultBytes));
}

inline constexpr std::size_t kMaxToolsListBytes = 8192;
inline constexpr int kReadDefaultLines = 200;
inline constexpr int kReadMaxLines = 500;
inline constexpr int kSearchDefaultHits = 50;
inline constexpr int kSearchMaxHits = 200;
inline constexpr int kSearchSnippetChars = 120;
inline constexpr int kGitLogDefault = 10;
inline constexpr int kGitLogMax = 30;
inline constexpr int kShellTimeoutDefaultSec = 30;
inline constexpr int kShellTimeoutMaxSec = 120;
inline constexpr int kDiagnoseMax = 100;
inline constexpr std::size_t kDescriptionMaxChars = 120;
