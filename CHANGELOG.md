# Changelog

All notable changes to this project will be documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- Native core tools (`read`, `edit`, `search`, `shell`, `project`, `git`, `build`, `test`, `diagnose`, `catalog`, `activate`, `deactivate`) with hashline edits, workspace jail, and 32 KiB result caps
- Profile detection (`auto|core|cpp|csharp|fullstack|all`) and `--tools` pin list so a C++ session advertises ≤ 12 tools
- Agent Skills discovery for `.agents/skills/` and `.toolsmith/skills/` (project then user) with an agentskills.io YAML frontmatter parser
- `skills` / `skill` tools (and `skill://` resources) behind the opt-in `skills` pack

### Changed
- `tools/list` is the advertised set only; collisions fail closed; chaining defaults off
- `echo` is hidden; command-skill rules are no longer dumped into tool descriptions
- SKILL.md parser keeps unquoted colons in `description` and nested `metadata` maps

### Fixed
- Duplicate tool names no longer silently overwrite the first registration

## [1.0.0] — 2026-05-06

### Added

#### Core Protocol
- Full MCP JSON-RPC 2.0 protocol handler with batched request support
- HTTP transport (cpp-httplib) with optional uWebSockets async mode
- Stdio transport with binary-safe Windows support
- `initialize`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/subscribe`, `resources/unsubscribe`, `completion/complete` methods
- Cancellation support via `notifications/cancelled` propagated to subprocesses

#### Tools & Commands
- `echo` — connectivity testing
- `llm` — LLM completion via LiteLLM proxy (multi-provider)
- `skill` — prompt-template engine with `{{variable}}` interpolation
- `remote` — composite command delegating to registered MCP servers

#### Plugin System
- **Script plugins** — Python, Node.js, C#, or any executable (per-call subprocess)
- **Native plugins** — compiled C/C++ shared libraries with hot-reload and `notifications/tools/list_changed`
- **Skill plugins** — `SKILL.md` Markdown prompt templates with frontmatter metadata
- **Stdio MCP plugins** — spawn child MCP servers, auto-discover tools via `tools/list`
- Tool chaining — chain tool calls without LLM round-trips (max depth 5)
- Parallel plugin discovery at startup via async futures

#### Security
- Token-bucket rate limiting per client IP
- API key authentication (optional)
- Input sanitization — payload size (1 MB), JSON nesting depth (32), string length caps
- Security headers on every HTTP response
- Path traversal protection for resource URIs

#### Plugins Included
- `git-tools` — Git operations (diff, log, status, conflicts)
- `github-tools` — GitHub API integration (issues, PRs, repos)
- `github-actions` — GitHub Actions workflow management
- `desktop-notification` — cross-platform desktop notifications (Windows WinRT, macOS osascript, Linux notify-send)
- `example-plugin` — reference native plugin (ping + base64_encode)
- `entrian-search` — Entrian source search skill
- `everything-search` — Everything file search skill
- `jira-tools` — Jira issue details, JQL search, comments skill

#### Integrations
- Claude Code registration via `claude mcp add`
- LiteLLM proxy tool provider (HTTP and stdio modes)
- C API shared library (`mcp_capi`) for FFI/P/Invoke interop
- C# wrapper (`McpClient.dll`) via dotnet build

#### Infrastructure
- CMake build system with FetchContent for all dependencies
- Cross-platform support: Windows (MSVC), Linux (GCC/Clang), macOS (Apple Clang)
- Version management via CMake `project(VERSION)` + generated `Version.h`
- CMake install targets for binary distribution
- Docker multi-stage build + docker-compose with LiteLLM sidecar
- GitHub Actions release pipeline (Linux, Windows, macOS archives + Docker push)
- 202 unit and integration tests (GTest)
