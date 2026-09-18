#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// OMP-style hashline: LINE|HASH|text. Hash is 4 lowercase hex chars
/// (low 16 bits of FNV-1a-32 over a whitespace-normalized line).
namespace Hashline {

std::string NormalizeLine(std::string_view line);
std::string LineHash(std::string_view line);

struct Line {
    int number = 0;          // 1-based
    std::string hash;
    std::string text;        // original line without trailing \n / \r
};

struct Hunk {
    int startLine = 0;
    std::string startHash;
    int endLine = 0;         // inclusive; 0 means startLine only (insert/replace one)
    std::string endHash;
    std::string content;     // replacement, may be multi-line
};

struct ParseError {
    std::string message;
};

std::vector<Line> ParseFile(std::string_view content);

std::string FormatRead(const std::vector<Line>& lines, int offset /*0-based*/,
                       int limit, bool hashes, bool& truncated, int& nextLine);

/// Verify hunks against current lines. On failure, writes nothing.
/// On success, returns the new file contents.
std::pair<std::string, std::string> ApplyHunks(const std::vector<Line>& lines,
                                               const std::vector<Hunk>& hunks);

}  // namespace Hashline
