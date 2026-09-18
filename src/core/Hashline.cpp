#include <core/Hashline.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace Hashline {

std::string NormalizeLine(std::string_view line) {
    std::string s(line);
    for (char& c : s) {
        if (c == '\t') c = ' ';
    }
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string LineHash(std::string_view line) {
    const std::string n = NormalizeLine(line);
    std::uint32_t h = 2166136261u;
    for (unsigned char c : n) {
        h ^= c;
        h *= 16777619u;
    }
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(h & 0xFFFFu));
    return buf;
}

std::vector<Line> ParseFile(std::string_view content) {
    std::vector<Line> lines;
    if (content.empty()) return lines;
    int num = 1;
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); ++i) {
        if (i < content.size() && content[i] != '\n') continue;
        if (i == content.size() && start == i) break; // trailing newline: no extra empty line
        std::string_view raw = content.substr(start, i - start);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        Line l;
        l.number = num++;
        l.text = std::string(raw);
        l.hash = LineHash(l.text);
        lines.push_back(std::move(l));
        start = i + 1;
    }
    return lines;
}

std::string FormatRead(const std::vector<Line>& lines, int offset, int limit,
                       bool hashes, bool& truncated, int& nextLine) {
    truncated = false;
    nextLine = 0;
    if (offset < 0) offset = 0;
    if (limit < 0) limit = 0;
    std::ostringstream oss;
    int emitted = 0;
    for (size_t i = static_cast<size_t>(offset); i < lines.size(); ++i) {
        if (emitted >= limit) {
            truncated = true;
            nextLine = lines[i].number;
            break;
        }
        const auto& l = lines[i];
        if (hashes) {
            oss << l.number << '|' << l.hash << '|' << l.text << '\n';
        } else {
            oss << l.number << '|' << l.text << '\n';
        }
        ++emitted;
    }
    return oss.str();
}

static const Line* FindLine(const std::vector<Line>& lines, int number) {
    if (number <= 0 || static_cast<size_t>(number) > lines.size()) return nullptr;
    const Line& l = lines[static_cast<size_t>(number) - 1];
    if (l.number != number) return nullptr;
    return &l;
}

std::pair<std::string, std::string> ApplyHunks(const std::vector<Line>& lines,
                                               const std::vector<Hunk>& hunks) {
    if (hunks.empty()) return {"", "hunks must not be empty"};

    std::vector<Hunk> ordered = hunks;
    std::sort(ordered.begin(), ordered.end(),
              [](const Hunk& a, const Hunk& b) { return a.startLine > b.startLine; });

    // Overlap check on original line numbers
    std::vector<Hunk> byStart = hunks;
    std::sort(byStart.begin(), byStart.end(),
              [](const Hunk& a, const Hunk& b) { return a.startLine < b.startLine; });
    for (size_t i = 1; i < byStart.size(); ++i) {
        int prevEnd = byStart[i - 1].endLine > 0 ? byStart[i - 1].endLine
                                                 : byStart[i - 1].startLine;
        if (byStart[i].startLine <= prevEnd) {
            return {"", "Overlapping hunks are not allowed"};
        }
    }

    auto working = lines;
    for (const auto& h : ordered) {
        if (h.startLine <= 0) return {"", "Invalid hunk start line"};
        int end = h.endLine > 0 ? h.endLine : h.startLine;
        if (end < h.startLine) return {"", "Hunk end line is before start line"};
        const Line* start = FindLine(working, h.startLine);
        if (!start) {
            return {"", "Stale hunk: start line " + std::to_string(h.startLine)
                            + " does not exist"};
        }
        if (start->hash != h.startHash) {
            return {"", "Stale hunk: expected " + std::to_string(h.startLine) + ":"
                            + h.startHash + " but file has " + std::to_string(start->number)
                            + ":" + start->hash};
        }
        if (!h.endHash.empty() && end != h.startLine) {
            const Line* el = FindLine(working, end);
            if (!el || el->hash != h.endHash) {
                return {"", "Stale hunk: end anchor mismatch at line "
                                + std::to_string(end)};
            }
        }
        // Replace [startLine, end] inclusive with content lines
        std::vector<Line> inserted;
        std::string cur;
        int num = h.startLine;
        auto flush = [&]() {
            Line l;
            l.number = num++;
            l.text = cur;
            l.hash = LineHash(cur);
            inserted.push_back(std::move(l));
            cur.clear();
        };
        for (char c : h.content) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                flush();
            } else {
                cur.push_back(c);
            }
        }
        if (!h.content.empty() && h.content.back() != '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            flush();
        } else if (h.content.empty()) {
            // delete the range: inserted stays empty
        }

        auto b = working.begin() + (h.startLine - 1);
        auto e = working.begin() + end;
        working.erase(b, e);
        working.insert(working.begin() + (h.startLine - 1),
                       inserted.begin(), inserted.end());
        for (size_t i = 0; i < working.size(); ++i) {
            working[i].number = static_cast<int>(i + 1);
        }
    }

    std::ostringstream oss;
    for (size_t i = 0; i < working.size(); ++i) {
        oss << working[i].text;
        if (i + 1 < working.size()) oss << '\n';
    }
    // Preserve trailing newline if original ended with one — ParseFile already
    // split on \n. If the last original line existed, join with \n between.
    if (!lines.empty()) oss << '\n';
    return {oss.str(), ""};
}

}  // namespace Hashline
