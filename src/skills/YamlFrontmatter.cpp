#include <skills/YamlFrontmatter.h>
#include <algorithm>
#include <cctype>
#include <sstream>

static std::string Trim(std::string s) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::string Unquote(std::string s) {
    s = Trim(std::move(s));
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"')
            || (s.front() == '\'' && s.back() == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    return s;
}

static int IndentOf(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 2;
        else break;
    }
    return n;
}

std::string YamlFrontmatter::GetString(const std::string& key) const {
    if (!fields.contains(key)) return "";
    const auto& v = fields.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number() || v.is_boolean()) return v.dump();
    return "";
}

std::vector<std::string> YamlFrontmatter::GetStringList(const std::string& key) const {
    std::vector<std::string> out;
    if (!fields.contains(key) || !fields.at(key).is_array()) return out;
    for (const auto& item : fields.at(key)) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

std::map<std::string, std::string> YamlFrontmatter::GetStringMap(const std::string& key) const {
    std::map<std::string, std::string> out;
    if (!fields.contains(key) || !fields.at(key).is_object()) return out;
    for (auto it = fields.at(key).begin(); it != fields.at(key).end(); ++it) {
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
        else if (it.value().is_number() || it.value().is_boolean())
            out[it.key()] = it.value().dump();
    }
    return out;
}

YamlFrontmatter ParseYamlFrontmatter(const std::string& content) {
    YamlFrontmatter fm;
    std::string text = content;
    // Strip UTF-8 BOM
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    if (text.size() < 3 || text.substr(0, 3) != "---") {
        fm.error = "SKILL.md has no valid YAML frontmatter";
        return fm;
    }

    auto closing = text.find("\n---", 3);
    if (closing == std::string::npos) {
        fm.error = "SKILL.md frontmatter is unclosed";
        return fm;
    }
    std::string yaml = text.substr(3, closing - 3);
    auto bodyStart = text.find('\n', closing + 4);
    fm.body = (bodyStart == std::string::npos) ? "" : text.substr(bodyStart + 1);
    // Trim leading/trailing blank lines from body
    while (!fm.body.empty() && (fm.body.front() == '\n' || fm.body.front() == '\r'))
        fm.body.erase(fm.body.begin());

    std::istringstream stream(yaml);
    std::string line;
    std::string currentKey;
    int currentIndent = 0;
    enum class Mode { Top, Map, List } mode = Mode::Top;

    auto flushKey = [&]() {};

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (Trim(line).empty()) continue;
        int indent = IndentOf(line);
        std::string trimmed = Trim(line);

        if (trimmed[0] == '#') continue;

        if (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            std::string item = Unquote(trimmed.substr(2));
            if (currentKey.empty()) continue;
            if (!fm.fields[currentKey].is_array())
                fm.fields[currentKey] = nlohmann::json::array();
            fm.fields[currentKey].push_back(item);
            mode = Mode::List;
            continue;
        }

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, colon));
        std::string value = Trim(trimmed.substr(colon + 1));

        if (indent > 0 && !currentKey.empty() && mode == Mode::Map) {
            if (!fm.fields[currentKey].is_object())
                fm.fields[currentKey] = nlohmann::json::object();
            fm.fields[currentKey][key] = Unquote(value);
            continue;
        }

        currentKey = key;
        currentIndent = indent;
        if (value.empty()) {
            mode = Mode::Map; // next indented keys or list items
            continue;
        }
        mode = Mode::Top;
        fm.fields[key] = Unquote(value);
        (void)flushKey;
        (void)currentIndent;
    }
    return fm;
}
