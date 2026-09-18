#include <commands/CoreTools.h>
#include <commands/ICommandStrategy.h>
#include <core/Hashline.h>
#include <core/Logger.h>
#include <core/ResultBudget.h>
#include <core/ToolProfile.h>
#include <plugins/SubprocessPipe.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

nlohmann::json Payload(const nlohmann::json& request) {
    if (request.contains("payload") && request["payload"].is_object())
        return request["payload"];
    return nlohmann::json::object();
}

nlohmann::json Ok(std::string content, bool truncated = false) {
    bool capTrunc = false;
    content = CapHead(std::move(content), capTrunc);
    return {
        {"status", "ok"},
        {"content", content},
        {"truncated", truncated || capTrunc}
    };
}

nlohmann::json Err(const std::string& message) {
    return {{"status", "error"}, {"error", message}};
}

std::future<nlohmann::json> Ready(nlohmann::json v) {
    return std::async(std::launch::deferred, [v = std::move(v)]() { return v; });
}

int ClampInt(int v, int lo, int hi, int fallback) {
    if (v == 0) return fallback;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool LooksBinary(std::string_view bytes) {
    const size_t n = std::min(bytes.size(), static_cast<size_t>(8192));
    return std::find(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n), '\0')
        != bytes.begin() + static_cast<std::ptrdiff_t>(n);
}

std::string ReadAll(const fs::path& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Cannot read file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string StripAnsi(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && (s[i] < '@' || s[i] > '~')) ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

struct ProcResult {
    std::string output;
    bool timedOut = false;
    bool started = false;
};

std::string PathUtf8(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

void AppendLines(SubprocessPipe& proc, std::string& output, int waitMs) {
    std::string line;
    while (proc.ReadLine(line, waitMs)) {
        output += line;
        output += '\n';
        if (output.size() > kMaxResultBytes * 4) break;
    }
}

ProcResult RunProc(const std::string& exe, const std::vector<std::string>& args,
                   int timeoutSec, const std::string& cwd = {}) {
    ProcResult r;
    std::unique_ptr<SubprocessPipe> proc;
    try {
        proc = SubprocessPipe::Spawn(exe, args, true, cwd);
    } catch (const std::exception& e) {
        r.output = e.what();
        return r;
    }
    r.started = true;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        const size_t before = r.output.size();
        AppendLines(*proc, r.output, 200);
        if (r.output.size() > kMaxResultBytes * 4) break;
        if (!proc->IsRunning()) break;
        if (r.output.size() == before)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (proc->IsRunning()) {
        proc->Kill();
        r.timedOut = true;
    }
    AppendLines(*proc, r.output, 50);
    r.output = StripAnsi(std::move(r.output));
    return r;
}

bool SkipDirName(const std::string& name) {
    return name == ".git" || name == "node_modules" || name == "build"
        || name == "dist" || name == ".cursor" || name == "__pycache__"
        || name == ".vs" || name == "out";
}

class CoreCommand : public ICommandStrategy {
public:
    CoreCommand(std::string name, std::string desc, nlohmann::json schema,
                std::shared_ptr<CoreRuntime> rt)
        : m_Name(std::move(name)), m_Desc(std::move(desc)),
          m_Schema(std::move(schema)), m_Rt(std::move(rt)) {}

    ToolMetadata GetMetadata() const override {
        ToolMetadata meta;
        meta.m_Name = m_Name;
        meta.m_Description = m_Desc.size() > kDescriptionMaxChars
            ? m_Desc.substr(0, kDescriptionMaxChars) : m_Desc;
        meta.m_InputSchema = m_Schema;
        meta.m_Source = ToolSource::BuiltIn;
        return meta;
    }

protected:
    std::string m_Name;
    std::string m_Desc;
    nlohmann::json m_Schema;
    std::shared_ptr<CoreRuntime> m_Rt;
};

class ReadCommand : public CoreCommand {
public:
    explicit ReadCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("read",
            "Read a file as LINE|HASH|text. Use offset/limit; hashes default on.",
            {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}}},
                    {"offset", {{"type", "integer"}}},
                    {"limit", {{"type", "integer"}}},
                    {"hashes", {{"type", "boolean"}}}
                }},
                {"required", nlohmann::json::array({"path"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        auto resolved = m_Rt->jail.Resolve(p.value("path", ""), false);
        if (!resolved.ok()) return Ready(Err(resolved.error));
        if (!fs::is_regular_file(resolved.path))
            return Ready(Err("Not a regular file: " + resolved.path.string()));
        std::string err;
        std::string bytes = ReadAll(resolved.path, err);
        if (!err.empty()) return Ready(Err(err));
        if (LooksBinary(bytes))
            return Ready(Err("Refusing to read binary file: " + resolved.path.string()));
        int offset = p.value("offset", 0);
        int limit = ClampInt(p.value("limit", kReadDefaultLines), 1, kReadMaxLines, kReadDefaultLines);
        if (p.contains("limit") && p["limit"].is_number_integer() && p["limit"].get<int>() <= 0)
            return Ready(Err("limit must be > 0"));
        if (offset < 0) return Ready(Err("offset must be >= 0"));
        bool hashes = p.value("hashes", true);
        auto lines = Hashline::ParseFile(bytes);
        if (offset > static_cast<int>(lines.size()))
            return Ready(Err("offset is past end of file"));
        bool truncated = false;
        int nextLine = 0;
        std::string body = Hashline::FormatRead(lines, offset, limit, hashes, truncated, nextLine);
        auto result = Ok(std::move(body), truncated);
        result["outside_workspace"] = resolved.outsideWorkspace;
        if (truncated) result["next_line"] = nextLine;
        return Ready(std::move(result));
    }
};

class EditCommand : public CoreCommand {
public:
    explicit EditCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("edit",
            "Hashline edit. Hunks use LINE:HASH anchors; stale hashes write nothing.",
            {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}}},
                    {"hunks", {{"type", "array"}}}
                }},
                {"required", nlohmann::json::array({"path", "hunks"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        auto resolved = m_Rt->jail.Resolve(p.value("path", ""), true);
        if (!resolved.ok()) return Ready(Err(resolved.error));
        if (!p.contains("hunks") || !p["hunks"].is_array())
            return Ready(Err("Missing required argument: hunks"));
        if (p["hunks"].empty())
            return Ready(Err("hunks must not be empty"));
        if (!fs::is_regular_file(resolved.path))
            return Ready(Err("File not found: " + resolved.path.string()));
        std::string err;
        std::string bytes = ReadAll(resolved.path, err);
        if (!err.empty()) return Ready(Err(err));
        if (LooksBinary(bytes))
            return Ready(Err("Refusing to edit binary file"));
        std::vector<Hashline::Hunk> hunks;
        for (const auto& h : p["hunks"]) {
            Hashline::Hunk hk;
            std::string start = h.value("start", "");
            std::string end = h.value("end", "");
            auto parseAnchor = [](const std::string& a, int& line, std::string& hash) {
                auto pos = a.find(':');
                if (pos == std::string::npos) return false;
                try { line = std::stoi(a.substr(0, pos)); }
                catch (...) { return false; }
                hash = a.substr(pos + 1);
                return line > 0 && hash.size() == 4;
            };
            if (!parseAnchor(start, hk.startLine, hk.startHash))
                return Ready(Err("Invalid hunk start anchor (want LINE:HASH)"));
            if (!end.empty() && !parseAnchor(end, hk.endLine, hk.endHash))
                return Ready(Err("Invalid hunk end anchor (want LINE:HASH)"));
            if (!h.contains("content") || !h["content"].is_string())
                return Ready(Err("Hunk missing content string"));
            hk.content = h["content"].get<std::string>();
            hunks.push_back(std::move(hk));
        }
        auto lines = Hashline::ParseFile(bytes);
        auto [next, applyErr] = Hashline::ApplyHunks(lines, hunks);
        if (!applyErr.empty()) return Ready(Err(applyErr));
        std::ofstream out(resolved.path, std::ios::binary | std::ios::trunc);
        if (!out) return Ready(Err("Cannot write file: " + resolved.path.string()));
        out << next;
        return Ready(Ok("Updated " + resolved.path.string() + " ("
                        + std::to_string(hunks.size()) + " hunks)"));
    }
};

class SearchCommand : public CoreCommand {
public:
    explicit SearchCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("search",
            "Search workspace files. Returns path:line: snippet, never file bodies.",
            {
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"}}},
                    {"path", {{"type", "string"}}},
                    {"max_hits", {{"type", "integer"}}}
                }},
                {"required", nlohmann::json::array({"pattern"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string pattern = p.value("pattern", "");
        if (pattern.empty()) return Ready(Err("Missing required argument: pattern"));
        std::string rel = p.value("path", "");
        auto resolved = m_Rt->jail.Resolve(rel.empty() ? "." : rel, true);
        if (!resolved.ok()) return Ready(Err(resolved.error));
        if (resolved.outsideWorkspace)
            return Ready(Err("search is jailed to the workspace"));
        int maxHits = ClampInt(p.value("max_hits", kSearchDefaultHits), 1, kSearchMaxHits,
                               kSearchDefaultHits);
        std::ostringstream oss;
        int hits = 0;
        bool truncated = false;
        std::error_code ec;
        fs::recursive_directory_iterator it(resolved.path,
            fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; it != end && !ec; it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (SkipDirName(it->path().filename().string())) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const auto file = it->path();
            auto ext = file.extension().string();
            if (ext == ".png" || ext == ".jpg" || ext == ".exe" || ext == ".dll"
                || ext == ".so" || ext == ".o" || ext == ".a") continue;
            std::string err;
            std::string bytes = ReadAll(file, err);
            if (!err.empty() || LooksBinary(bytes)) continue;
            auto relPath = file.lexically_relative(m_Rt->jail.Root()).generic_string();
            auto lines = Hashline::ParseFile(bytes);
            for (const auto& line : lines) {
                if (line.text.find(pattern) == std::string::npos
                    && relPath.find(pattern) == std::string::npos) continue;
                if (hits >= maxHits) { truncated = true; break; }
                std::string snip = line.text;
                if (snip.size() > static_cast<size_t>(kSearchSnippetChars))
                    snip.resize(static_cast<size_t>(kSearchSnippetChars));
                oss << relPath << ':' << line.number << ": " << snip << '\n';
                ++hits;
            }
            if (truncated) break;
        }
        auto result = Ok(hits == 0 ? "No matches.\n" : oss.str(), truncated);
        result["hits"] = hits;
        return Ready(std::move(result));
    }
};

class ShellCommand : public CoreCommand {
public:
    explicit ShellCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("shell",
            "Run a command in the workspace. Timeout 30s default, 120s max. No background.",
            {
                {"type", "object"},
                {"properties", {
                    {"command", {{"type", "string"}}},
                    {"cwd", {{"type", "string"}}},
                    {"timeout", {{"type", "integer"}}}
                }},
                {"required", nlohmann::json::array({"command"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string command = p.value("command", "");
        if (command.empty()) return Ready(Err("Missing required argument: command"));
        std::string cwdArg = p.value("cwd", "");
        auto resolved = m_Rt->jail.Resolve(cwdArg.empty() ? "." : cwdArg, true);
        if (!resolved.ok()) return Ready(Err(resolved.error));
        int timeout = ClampInt(p.value("timeout", kShellTimeoutDefaultSec), 1,
                               kShellTimeoutMaxSec, kShellTimeoutDefaultSec);
        const std::string cwd = PathUtf8(resolved.path);
#ifdef _WIN32
        // cwd is passed to CreateProcess. Do not wrap `cd /d ... &&` — subprocess.h
        // re-quotes args with spaces and cmd.exe then strips quotes incorrectly.
        auto proc = RunProc("cmd.exe", {"/s", "/c", command}, timeout, cwd);
#else
        auto proc = RunProc("/bin/sh", {"-c", command}, timeout, cwd);
#endif
        if (!proc.started) return Ready(Err("Failed to start shell: " + proc.output));
        bool trunc = false;
        std::string body = CapTail(proc.output, trunc);
        if (proc.timedOut) {
            auto r = Err("Command timed out after " + std::to_string(timeout) + "s");
            r["content"] = body;
            r["truncated"] = trunc;
            r["exit_code"] = -1;
            return Ready(std::move(r));
        }
        auto r = Ok(body, trunc);
        r["exit_code"] = 0;
        return Ready(std::move(r));
    }
};

class ProjectCommand : public CoreCommand {
public:
    explicit ProjectCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("project",
            "Compact language and toolchain snapshot of the workspace root.",
            {{"type", "object"}, {"properties", nlohmann::json::object()}},
            std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        const auto& root = m_Rt->jail.Root();
        std::ostringstream oss;
        oss << "root: " << root.generic_string() << '\n';
        oss << "profile: " << m_Rt->profile << '\n';
        oss << "cmake: " << (fs::exists(root / "CMakeLists.txt") ? "yes" : "no") << '\n';
        oss << "compile_commands: "
            << (fs::exists(root / "compile_commands.json") ? "yes" : "no") << '\n';
        bool sln = false, csproj = false;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
            auto ext = it->path().extension().string();
            if (ext == ".sln") sln = true;
            if (ext == ".csproj") csproj = true;
        }
        oss << "dotnet_sln: " << (sln ? "yes" : "no") << '\n';
        oss << "csproj: " << (csproj ? "yes" : "no") << '\n';
        return Ready(Ok(oss.str()));
    }
};

class GitCommand : public CoreCommand {
public:
    explicit GitCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("git",
            "Read-only git: status, diff, log, blame, conflicts. Writes are pack git_write.",
            {
                {"type", "object"},
                {"properties", {
                    {"action", {{"type", "string"},
                                {"enum", nlohmann::json::array(
                                    {"status", "diff", "log", "blame", "conflicts"})}}},
                    {"path", {{"type", "string"}}},
                    {"full", {{"type", "boolean"}}},
                    {"limit", {{"type", "integer"}}}
                }},
                {"required", nlohmann::json::array({"action"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string action = p.value("action", "");
        const std::string root = m_Rt->jail.Root().string();
        if (action == "status") {
            auto proc = RunProc("git", {"-C", root, "status", "-sb", "--untracked-files=normal"}, 15);
            if (!proc.started) return Ready(Err("git not available"));
            return Ready(Ok(proc.output, false));
        }
        if (action == "diff") {
            bool full = p.value("full", false);
            std::string path = p.value("path", "");
            if (full && path.empty())
                return Ready(Err("full diff requires path"));
            std::vector<std::string> args = {"-C", root, "diff", "--stat"};
            if (full) {
                args = {"-C", root, "diff", "--", path};
            }
            auto proc = RunProc("git", args, 20);
            if (!proc.started) return Ready(Err("git not available"));
            bool trunc = false;
            return Ready(Ok(CapHead(proc.output, trunc), trunc));
        }
        if (action == "log") {
            int limit = ClampInt(p.value("limit", kGitLogDefault), 1, kGitLogMax, kGitLogDefault);
            auto proc = RunProc("git", {"-C", root, "log", "-n", std::to_string(limit),
                                        "--oneline"}, 15);
            if (!proc.started) return Ready(Err("git not available"));
            return Ready(Ok(proc.output));
        }
        if (action == "blame") {
            std::string path = p.value("path", "");
            if (path.empty()) return Ready(Err("blame requires path"));
            auto resolved = m_Rt->jail.Resolve(path, false);
            if (!resolved.ok()) return Ready(Err(resolved.error));
            auto proc = RunProc("git", {"-C", root, "blame", "-L", "1,40",
                                        "--", resolved.path.string()}, 15);
            if (!proc.started) return Ready(Err("git not available"));
            return Ready(Ok(proc.output));
        }
        if (action == "conflicts") {
            auto proc = RunProc("git", {"-C", root, "diff", "--name-only",
                                        "--diff-filter=U"}, 15);
            if (!proc.started) return Ready(Err("git not available"));
            return Ready(Ok(proc.output.empty() ? "No conflicted files.\n" : proc.output));
        }
        return Ready(Err("Unknown git action: " + action
                         + " (want status|diff|log|blame|conflicts)"));
    }
};

class BuildCommand : public CoreCommand {
public:
    explicit BuildCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("build",
            "Build the workspace (CMake/MSBuild/dotnet). Returns compact errors, not full logs.",
            {{"type", "object"}, {"properties", {
                {"verbose", {{"type", "boolean"}}}
            }}}, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        bool verbose = p.value("verbose", false);
        const auto& root = m_Rt->jail.Root();
        if (fs::exists(root / "CMakeLists.txt")) {
            auto proc = RunProc("cmake", {"--build", (root / "build").string(),
                                          "--parallel"}, 60);
            if (!proc.started) {
                return Ready(Err("cmake not available on PATH"));
            }
            bool trunc = false;
            std::string body = verbose ? proc.output : CapTail(proc.output, trunc);
            auto r = Ok(body, trunc || proc.timedOut);
            if (proc.timedOut) r["error_note"] = "build timed out";
            return Ready(std::move(r));
        }
        std::error_code ec;
        for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
            auto ext = it->path().extension().string();
            if (ext == ".sln" || ext == ".csproj") {
                auto proc = RunProc("dotnet", {"build", it->path().string(),
                                              "--nologo", "-v:q"}, 60);
                if (!proc.started) return Ready(Err("dotnet not available on PATH"));
                bool trunc = false;
                return Ready(Ok(CapTail(proc.output, trunc), trunc));
            }
        }
        return Ready(Err("No CMakeLists.txt, compile_commands.json, .sln, or .csproj in workspace"));
    }
};

class TestCommand : public CoreCommand {
public:
    explicit TestCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("test",
            "Run tests (ctest or dotnet test). Compact failures only.",
            {{"type", "object"}, {"properties", nlohmann::json::object()}},
            std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        const auto& root = m_Rt->jail.Root();
        if (fs::exists(root / "build")) {
            auto proc = RunProc("ctest", {"--test-dir", (root / "build").string(),
                                          "--output-on-failure"}, 60);
            if (proc.started) {
                bool trunc = false;
                return Ready(Ok(CapTail(proc.output, trunc), trunc));
            }
        }
        std::error_code ec;
        for (fs::directory_iterator it(root, ec); it != fs::directory_iterator() && !ec; ++it) {
            if (it->path().extension() == ".csproj" || it->path().extension() == ".sln") {
                auto proc = RunProc("dotnet", {"test", it->path().string(), "--nologo"}, 60);
                if (!proc.started) return Ready(Err("dotnet not available on PATH"));
                bool trunc = false;
                return Ready(Ok(CapTail(proc.output, trunc), trunc));
            }
        }
        return Ready(Err("No test runner found (expected build/ + ctest or a .csproj/.sln)"));
    }
};

class DiagnoseCommand : public CoreCommand {
public:
    explicit DiagnoseCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("diagnose",
            "Compiler diagnostics as file:line:col:severity:message. Max 100.",
            {
                {"type", "object"},
                {"properties", {{"path", {{"type", "string"}}}}}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string path = p.value("path", "");
        if (!path.empty()) {
            auto resolved = m_Rt->jail.Resolve(path, false);
            if (!resolved.ok()) return Ready(Err(resolved.error));
            if (!fs::is_regular_file(resolved.path))
                return Ready(Err("Not a file: " + resolved.path.string()));
#ifdef _WIN32
            auto proc = RunProc("cl", {resolved.path.string(), "/nologo", "/Zs"}, 30);
#else
            auto proc = RunProc("c++", {"-fsyntax-only", "-Wall", resolved.path.string()}, 30);
#endif
            if (!proc.started) {
                return Ready(Err("No compiler on PATH for diagnose"));
            }
            bool trunc = false;
            std::string body = CapHead(proc.output, trunc);
            int n = 0;
            std::istringstream in(body);
            std::ostringstream out;
            std::string line;
            while (std::getline(in, line) && n < kDiagnoseMax) {
                if (line.find("error") == std::string::npos
                    && line.find("warning") == std::string::npos) continue;
                out << line << '\n';
                ++n;
            }
            auto r = Ok(n == 0 ? "No diagnostics.\n" : out.str(), trunc);
            r["count"] = n;
            return Ready(std::move(r));
        }
        return Ready(Err("diagnose requires path to a source file"));
    }
};

class CatalogCommand : public CoreCommand {
public:
    explicit CatalogCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("catalog",
            "List installed packs and whether they are advertised.",
            {{"type", "object"}, {"properties", nlohmann::json::object()}},
            std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        nlohmann::json packs = nlohmann::json::array();
        struct Row { const char* id; const char* desc; };
        const Row rows[] = {
            {"core", "Native read/edit/search/shell/project"},
            {"git", "Read-only git"},
            {"git_write", "Destructive git (commit/sync/reset/push)"},
            {"skills", "Agent Skills via .agents/skills"},
            {"llm", "LiteLLM completion tool"},
            {"remote", "Composite remote MCP"},
            {"github", "GitHub CLI pack"},
            {"jira", "Jira pack"},
            {"unity", "Unity pack"},
            {"unreal", "Unreal pack"},
        };
        std::lock_guard<std::mutex> lock(m_Rt->mutex);
        for (const auto& row : rows) {
            bool active = false;
            if (std::string(row.id) == "core") active = m_Rt->advertised.count("read") > 0;
            else if (std::string(row.id) == "git") active = m_Rt->advertised.count("git") > 0;
            else if (std::string(row.id) == "skills")
                active = m_Rt->advertised.count("skills") > 0;
            else if (std::string(row.id) == "llm") active = m_Rt->advertised.count("llm") > 0;
            else if (std::string(row.id) == "remote")
                active = m_Rt->advertised.count("remote") > 0;
            packs.push_back({
                {"id", row.id},
                {"description", row.desc},
                {"active", active}
            });
        }
        return Ready(Ok(packs.dump(2)));
    }
};

class ActivateCommand : public CoreCommand {
public:
    explicit ActivateCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("activate",
            "Advertise a pack for this session. Does not rewrite config.",
            {
                {"type", "object"},
                {"properties", {{"pack", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"pack"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string pack = p.value("pack", "");
        if (pack.empty()) return Ready(Err("Missing required argument: pack"));
        if (pack == "skills") {
            std::lock_guard<std::mutex> lock(m_Rt->mutex);
            m_Rt->advertised.insert("skills");
            m_Rt->advertised.insert("skill");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Activated pack skills"));
        }
        if (pack == "llm") {
            std::lock_guard<std::mutex> lock(m_Rt->mutex);
            m_Rt->advertised.insert("llm");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Activated pack llm"));
        }
        if (pack == "remote") {
            std::lock_guard<std::mutex> lock(m_Rt->mutex);
            m_Rt->advertised.insert("remote");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Activated pack remote"));
        }
        if (pack == "git") {
            std::lock_guard<std::mutex> lock(m_Rt->mutex);
            m_Rt->advertised.insert("git");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Activated pack git"));
        }
        if (ToolProfile::IsOptionalPack(pack) || pack == "core") {
            if (pack == "core") return Ready(Err("core pack is always on"));
            return Ready(Err("Pack '" + pack + "' is not loaded in this process. "
                             "Pin it with --profile all or tools.enable in config."));
        }
        return Ready(Err("Unknown pack: " + pack));
    }
};

class DeactivateCommand : public CoreCommand {
public:
    explicit DeactivateCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("deactivate",
            "Remove a pack from the advertised set. Core tools cannot be removed.",
            {
                {"type", "object"},
                {"properties", {{"pack", {{"type", "string"}}},
                                {"tool", {{"type", "string"}}}}},
                {"required", nlohmann::json::array()}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        auto p = Payload(request);
        std::string pack = p.value("pack", "");
        std::string tool = p.value("tool", "");
        std::lock_guard<std::mutex> lock(m_Rt->mutex);
        if (!tool.empty()) {
            if (m_Rt->locked.count(tool))
                return Ready(Err("Cannot deactivate core tool: " + tool));
            if (!m_Rt->advertised.count(tool))
                return Ready(Err("Tool is not advertised: " + tool));
            m_Rt->advertised.erase(tool);
            m_Rt->SyncAdvertised();
            return Ready(Ok("Deactivated tool " + tool));
        }
        if (pack.empty()) return Ready(Err("Missing required argument: pack or tool"));
        if (pack == "core") return Ready(Err("Cannot deactivate core pack"));
        if (pack == "skills") {
            m_Rt->advertised.erase("skills");
            m_Rt->advertised.erase("skill");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Deactivated pack skills"));
        }
        if (pack == "git") {
            if (m_Rt->locked.count("git"))
                return Ready(Err("Cannot deactivate git in this profile"));
            m_Rt->advertised.erase("git");
            m_Rt->SyncAdvertised();
            return Ready(Ok("Deactivated pack git"));
        }
        m_Rt->advertised.erase(pack);
        m_Rt->SyncAdvertised();
        return Ready(Ok("Deactivated pack " + pack));
    }
};

class SkillsCatalogCommand : public CoreCommand {
public:
    explicit SkillsCatalogCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("skills",
            "Agent Skills catalog: name and description only.",
            {{"type", "object"}, {"properties", nlohmann::json::object()}},
            std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json&) override {
        if (!m_Rt->advertised.count("skills"))
            return Ready(Err("skills pack is not advertised; call activate pack=skills"));
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : m_Rt->skills.skills) {
            arr.push_back({
                {"name", s.m_Name},
                {"description", s.m_Description},
                {"location", s.m_Location}
            });
        }
        return Ready(Ok(arr.dump(2)));
    }
};

class SkillLoadCommand : public CoreCommand {
public:
    explicit SkillLoadCommand(std::shared_ptr<CoreRuntime> rt)
        : CoreCommand("skill",
            "Load one Agent Skill body (progressive disclosure tier 2).",
            {
                {"type", "object"},
                {"properties", {{"name", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"name"})}
            }, std::move(rt)) {}

    std::future<nlohmann::json> ExecuteAsync(const nlohmann::json& request) override {
        if (!m_Rt->advertised.count("skill") && !m_Rt->advertised.count("skills"))
            return Ready(Err("skills pack is not advertised; call activate pack=skills"));
        auto p = Payload(request);
        auto body = LoadAgentSkillBody(m_Rt->skills, p.value("name", ""));
        if (!body.error.empty()) return Ready(Err(body.error));
        bool trunc = false;
        auto r = Ok(CapHead(body.body, trunc), trunc);
        r["root"] = body.root;
        r["allowed_tools"] = body.allowedTools;
        return Ready(std::move(r));
    }
};

}  // namespace

void RegisterCoreTools(CommandRegistry& registry,
                       const std::shared_ptr<CoreRuntime>& runtime) {
    runtime->registry = &registry;
    for (const char* t : ToolProfile::kCoreTools) runtime->locked.insert(t);
    for (const char* t : ToolProfile::kLanguageExtra) {
        if (runtime->advertised.count(t)) runtime->locked.insert(t);
    }
    registry.RegisterCommand("read", std::make_shared<ReadCommand>(runtime));
    registry.RegisterCommand("edit", std::make_shared<EditCommand>(runtime));
    registry.RegisterCommand("search", std::make_shared<SearchCommand>(runtime));
    registry.RegisterCommand("shell", std::make_shared<ShellCommand>(runtime));
    registry.RegisterCommand("project", std::make_shared<ProjectCommand>(runtime));
    registry.RegisterCommand("git", std::make_shared<GitCommand>(runtime));
    registry.RegisterCommand("build", std::make_shared<BuildCommand>(runtime));
    registry.RegisterCommand("test", std::make_shared<TestCommand>(runtime));
    registry.RegisterCommand("diagnose", std::make_shared<DiagnoseCommand>(runtime));
    registry.RegisterCommand("catalog", std::make_shared<CatalogCommand>(runtime));
    registry.RegisterCommand("activate", std::make_shared<ActivateCommand>(runtime));
    registry.RegisterCommand("deactivate", std::make_shared<DeactivateCommand>(runtime));
    registry.RegisterCommand("skills", std::make_shared<SkillsCatalogCommand>(runtime));
    registry.RegisterCommand("skill", std::make_shared<SkillLoadCommand>(runtime));
}
