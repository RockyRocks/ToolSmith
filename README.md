# ToolSmith

<img src=".\images\Logo.png" align="right" width="120" alt="Project Logo" />

**Forge any tool. Scale any workflow.**

An enterprise-grade **Model Context Protocol (MCP)** server implemented in C++20. It exposes LLM tools, prompt-driven skills, and remote server discovery over both HTTP and stdio transports, following the MCP JSON-RPC 2.0 specification.

<br clear="right"/>

---

## About

This project is a production-ready MCP server that bridges any LLM — Claude, GPT, Gemini, or open-source models — with custom tools, prompt-driven skills, and remote server networks through a single, unified protocol layer.

It is built around three ideas:

- **One protocol, every LLM.** The server speaks MCP JSON-RPC 2.0 over both HTTP and stdio. Any client that implements the protocol gets access to all registered tools — built-in commands, JSON skills, SKILL.md prompt templates, native C/C++ plugins, and script plugins in Python, Node.js, or C# — without per-model configuration.

- **Extend without recompiling.** The plugin system supports three extension models — script plugins (subprocess per call), native plugins (shared libraries with hot-reload), and skill plugins (Markdown prompt templates) — so new tools can be added at runtime by dropping files into the `plugins/` directory. A background watcher detects new native plugins and pushes `notifications/tools/list_changed` so connected clients refresh automatically.

- **Safe by default.** Every request passes through rate limiting, API key validation, input sanitization, and security headers before reaching any tool. Native plugins run inside a fault-isolation wrapper with exception catching, 30-second timeouts, and a circuit breaker. A misbehaving plugin cannot crash the server or block other tools.

The server integrates directly with [Claude Code](https://claude.ai/code) as a registered MCP tool source and with [LiteLLM](https://github.com/BerriAI/litellm) as a proxy-level tool provider, making all registered tools available to every model routed through the proxy. It also exposes a C API for FFI/P/Invoke interop with .NET and other languages.

---

## Features

### MCP Tools

Default `--profile auto` (CMake/C# tree → `cpp`/`csharp`/`fullstack`) advertises a small native set (≤ 12 tools):

| Tool | Description |
| ---- | ----------- |
| `read` | File as `LINE\|HASH\|text` (default 200 lines, cap 500) |
| `edit` | Hashline hunks; stale hashes write nothing |
| `search` | Workspace-jailed `path:line:` snippets |
| `shell` | One foreground command in the jail (30s default) |
| `project` | Compact language/toolchain snapshot |
| `git` | Read-only status/diff/log/blame/conflicts (language profiles) |
| `build` / `test` / `diagnose` | Compact compiler/test output (language profiles) |
| `catalog` / `activate` / `deactivate` | Opt-in packs (`skills`, `llm`, `jira`, …) |

`echo`, `llm`, `remote`, JSON skills, and script plugins are **not** in the default advertised set. Use `--profile all` for the previous full list, or `catalog` + `activate`. Agent Skills live under `.agents/skills/<name>/SKILL.md` ([spec](https://agentskills.io/specification)).

CLI: `--stdio --profile cpp --tools read,edit,search --workspace PATH`. Env: `TOOLSMITH_PROFILE`, `TOOLSMITH_TOOLS`, `TOOLSMITH_WORKSPACE`.

### Dual Transport

- **HTTP** — Default transport powered by [cpp-httplib](https://github.com/yhirose/cpp-httplib); optional high-performance async mode via [uWebSockets](https://github.com/uNetworking/uWebSockets) (`-DUSE_UWS=ON`).
- **Stdio** — Full MCP JSON-RPC 2.0 over stdin/stdout (`--stdio` flag). Binary-safe on Windows.

### HTTP REST API

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/mcp` | POST | Main MCP protocol handler (protected) |
| `/health` | GET | Health check |
| `/skills` | GET | List available skills |
| `/servers` | GET | List remote MCP servers |
| `/commands` | GET | List registered commands |

### Security

- **Rate Limiting** — Token-bucket algorithm per client IP
- **API Key Authentication** — Optional header-based API key validation
- **Input Sanitization** — Payload size limits (1 MB default), JSON nesting depth (max 32), string length caps
- **Security Headers** — Standard HTTP security headers on every response

### Skill Engine

Define reusable prompt templates as JSON files in the `skills/` directory:

```jsonc
{
  "name": "code_review",
  "description": "Review code for quality and bugs",
  "prompt_template": "Review the following {{language}} code:\n{{code}}",
  "default_model": "claude-sonnet",
  "required_variables": ["language", "code"]
}
```

### Remote Server Discovery

Route tool calls to other MCP servers via `config/mcp_servers.json`. Each server entry declares capabilities, priority, and timeout — the composite command picks the best match automatically.

### C API & C# Interop

A shared-library C API (`toolsmith_capi`) exposes lifecycle, command, LLM, skill, and discovery functions for P/Invoke from .NET or any FFI-capable language.

---

## Supported Platforms

| OS | Compiler | Minimum Version |
| -- | -------- | --------------- |
| **Windows** | MSVC (Visual Studio 2022) | v17+ with Desktop C++ workload |
| **Linux** | GCC | 10+ |
| **Linux** | Clang | 10+ |
| **macOS** | Apple Clang (Xcode) | 14+ |

> All platforms require **CMake 3.15+** and a compiler with **C++20** support.

---

## Architecture

See **[ARCHITECTURE.md](ARCHITECTURE.md)** for the full architecture documentation, including:

- High-level component diagram (Mermaid)
- Key design decisions and rationale
- Request flow sequences (single call and parallel `tools/call_batch`)
- Detailed directory structure

---

## Dependencies

All fetched automatically via CMake `FetchContent`:

| Library | Version | Purpose |
| ------- | ------- | ------- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.2 | JSON serialization |
| [json-schema-validator](https://github.com/pboettch/json-schema-validator) | 2.3.0 | JSON Schema validation |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.26.0 | HTTP client & server |
| [uWebSockets](https://github.com/uNetworking/uWebSockets) | 20.47.0 | Async WebSocket server (optional) |
| [Google Test](https://github.com/google/googletest) | 1.14.0 | Unit testing (default) |
| [Catch2](https://github.com/catchorg/Catch2) | 3.5.2 | Unit testing (alternative) |

**Platform-specific:** pthreads (Linux), CoreFoundation & CFNetwork (macOS).

---

## Quick Start

### Build from Source

```bash
# Clone & configure
cd MCP_Open
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Run
./build/toolsmith                # HTTP mode (default)
./build/toolsmith --stdio        # Stdio / MCP mode
```

#### Windows (Visual Studio)

```powershell
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\Release\toolsmith.exe
```

### Docker

```bash
# Build and run
docker build -t toolsmith .
docker run --rm -i toolsmith --stdio

# Or use docker-compose (includes LiteLLM sidecar)
docker compose up
```

### Pre-built Binaries

Download platform archives from [GitHub Releases](../../releases). Extract and run `bin/toolsmith`.

### CMake Options

| Option | Default | Description |
| ------ | ------- | ----------- |
| `USE_UWS` | `OFF` | Use uWebSockets async server |
| `BUILD_CSHARP` | `ON` | Build C# wrapper (requires .NET 8 SDK) |
| `TEST_FRAMEWORK` | `GTest` | `GTest` or `Catch2` |

---

## Integrations

### Claude Code

Register the server with [Claude Code](https://claude.ai/code) so it is available as a tool source in your AI-assisted development workflow.

#### Register with `claude mcp add`

##### Linux / macOS

```bash
claude mcp add toolsmith -- /path/to/MCP_Open/build/toolsmith
```

##### Windows

```powershell
claude mcp add toolsmith -- "C:\path\to\MCP_Open\build\Release\toolsmith.exe"
```

With LiteLLM API keys (required for `llm` and `skill` tools):

```bash
# Linux / macOS
claude mcp add toolsmith \
  --env ANTHROPIC_API_KEY=sk-ant-... \
  --env LITELLM_MASTER_KEY=sk-litellm-... \
  -- /path/to/build/toolsmith

# Windows
claude mcp add toolsmith ^
  --env ANTHROPIC_API_KEY=sk-ant-... ^
  --env LITELLM_MASTER_KEY=sk-litellm-... ^
  -- "C:\path\to\build\Release\toolsmith.exe"
```

With MCP server API key (if `api_key` is set in `config/mcp_config.json`):

```bash
claude mcp add toolsmith --env MCP_API_KEY=your-api-key -- /path/to/build/toolsmith
```

#### Scope Options

| Scope | Flag | Stored in | When to use |
| ----- | ---- | --------- | ----------- |
| `local` (default) | _(omit)_ | `.claude/` in the current directory | Personal, single-project use |
| `project` | `--scope project` | `.mcp.json` alongside source code | Shared with the team via VCS |
| `user` | `--scope user` | `~/.claude/` | Available across all projects on this machine |

```bash
# Share with team via source control
claude mcp add --scope project toolsmith -- /path/to/build/toolsmith

# Register globally for all projects
claude mcp add --scope user toolsmith -- /path/to/build/toolsmith
```

#### Verify

```bash
claude mcp list           # Confirm toolsmith appears
claude mcp get toolsmith   # Inspect connection details
```

Or from within a Claude Code session: `/mcp`

#### Available Tools After Registration

| Tool | Example prompt |
| ---- | -------------- |
| `echo` | "Use the echo tool to test the MCP connection" |
| `llm` | "Use the llm tool to summarize this file" |
| `skill` | "Run the code_review skill on this function" |
| `remote` | "Use the remote tool to query my other registered MCP servers" |

---

### LiteLLM Proxy (All LLMs)

Expose this server's tools to **every LLM** routed through your LiteLLM proxy — no per-client configuration required.

#### Step 1 — Start the MCP Server in HTTP mode

```bash
# Linux / macOS
./build/toolsmith          # Listens on the port in config/mcp_config.json (default 8080)

# Windows
build\Release\toolsmith.exe
```

#### Step 2 — Add to `litellm/litellm_config.yaml`

```yaml
mcp_servers:
  toolsmith:
    url: "http://localhost:8080"   # Match the port in config/mcp_config.json
    transport: "http"
    allow_all_keys: true           # Available to every API key / team
```

Alternatively, let the proxy spawn the process directly via stdio:

```yaml
mcp_servers:
  toolsmith:
    transport: "stdio"
    command: "/path/to/build/toolsmith"
    args: ["--stdio"]
    env:
      MCP_API_KEY: os.environ/MCP_API_KEY
    allow_all_keys: true
```

#### Step 3 — Start the LiteLLM Proxy

```bash
# Linux / macOS
cd litellm && ./start_proxy.sh

# Windows
cd litellm && .\start_proxy.ps1
```

#### Step 4 — Call Tools via Any LLM

```bash
curl http://localhost:4000/v1/chat/completions \
  -H "Authorization: Bearer $LITELLM_MASTER_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "claude-sonnet",
    "messages": [{"role": "user", "content": "Summarize this code using available tools"}],
    "tools": [{"type": "mcp", "server_url": "litellm_proxy", "require_approval": "never"}]
  }'
```

All models in `litellm_config.yaml` (`claude-sonnet`, `gpt-4o`, `gemini-pro`, …) automatically have access to all registered tools including promoted skill and plugin tools.

---

## Plugins

See **[plugins/README.md](plugins/README.md)** for the full plugin documentation, including:

- **Script Plugins** — Python, Node.js, C#, or any executable (per-call subprocess)
- **Native Plugins** — Compiled C/C++ shared libraries with hot-reload
- **Skill Plugins** — Prompt-template tools defined as `SKILL.md` files
- **Tool Chaining** — Chain tool calls without LLM round-trips (max depth 5)

Available plugins:

| Plugin | Type | Description |
| ------ | ---- | ----------- |
| [`git-tools`](plugins/git-tools/) | Script | Git operations |
| [`github-tools`](plugins/github-tools/) | Script | GitHub API integration |
| [`github-actions`](plugins/github-actions/) | Script | GitHub Actions management |
| [`desktop-notification`](plugins/desktop-notification/) | Native | Desktop notifications |
| [`example-plugin`](plugins/example-plugin/) | Native | Reference plugin (ping + base64_encode) |
| [`entrian-search`](plugins/entrian-search/) | Skill | Entrian source search |
| [`everything-search`](plugins/everything-search/) | Skill | Everything file search |
| [`jira-tools`](plugins/jira-tools/) | Skill | Jira issue details, JQL search, comments |

---

## Configuration

| File | Purpose |
| ---- | ------- |
| `config/mcp_config.json` | Server port, thread pool size, rate limits, auth, LiteLLM URL |
| `config/mcp_servers.json` | Remote MCP server endpoints for discovery |
| `skills/*.json` | Skill prompt template definitions |
| `litellm/litellm_config.yaml` | LiteLLM proxy model routing |

Copy the `.example` files and edit to taste:

```bash
cp config/mcp_config.json.example config/mcp_config.json
cp config/mcp_servers.json.example config/mcp_servers.json
```

---

## Testing

```bash
# Run all tests
ctest --test-dir build -C Release --output-on-failure

# Or run directly
./build/unit_tests            # Linux / macOS
build\Release\unit_tests.exe  # Windows
```

Test suites cover: protocol handling, command registry, input sanitization, rate limiting, configuration, skill engine, server discovery, stdio transport, native plugin system, script plugin adapter, and script plugin loader.

12 integration tests are conditionally skipped when Python is not on `PATH` — they run automatically once Python is installed.

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

**Attribution requirement:** If you use this MCP server or any part of its codebase in your project, you must give appropriate credit to the original author by including the following notice in your documentation or source:

> ToolSmith by Rakesh Kumar Raparla — [github.com/rraparla](https://github.com/rraparla)
