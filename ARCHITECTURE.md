# Architecture

## High-Level Component Diagram

```mermaid
graph TD
    subgraph Entry
        MAIN[server_entry.cpp]
    end

    subgraph Configuration
        CFG[Config]
    end

    subgraph Transport Layer
        HTTP[HTTP Server<br/>httplib / uWebSockets]
        STDIO[Stdio Transport<br/>JSON-RPC 2.0<br/>+ PushNotification]
    end

    subgraph Security Layer
        RL[Rate Limiter]
        AK[API Key Validator]
        IS[Input Sanitizer]
        SH[Security Headers]
    end

    subgraph Core
        PH[Protocol Handler]
        CR[Command Registry<br/>installed vs advertised]
    end

    subgraph Built-in Commands
        ECHO[Echo Command<br/>BuiltIn]
        LLM_CMD[LLM Command<br/>BuiltIn]
        REMOTE_CMD[Remote / Composite Command<br/>BuiltIn]
        SKILL_CMD[Skill Command<br/>BuiltIn — hidden]
    end

    subgraph Skill Tools
        STA[SkillToolAdapter ×N<br/>JsonSkill / Plugin]
        SE[Skill Engine]
        PL[PluginLoader<br/>SKILL.md]
    end

    subgraph Native Plugin System
        NPL[NativePluginLoader]
        WATCH[Plugin Watcher<br/>background thread]
        DLP[DlPlugin<br/>LoadLibrary / dlopen]
        NPA[NativePluginAdapter ×N<br/>NativePlugin — fault-isolated]
    end

    subgraph Script Plugin System
        SPL[ScriptPluginLoader]
        SPA[ScriptPluginAdapter ×N<br/>ScriptPlugin — per-call subprocess]
        SCRIPT[Python / Node.js / C# / Executable<br/>plugin scripts]
    end

    subgraph Engines
        LP[LiteLLM Provider]
        MSR[MCP Server Registry]
        HC[HTTP Client]
    end

    subgraph External
        LITELLM[(LiteLLM Proxy)]
        REMOTE_SRV[(Remote MCP Servers)]
        PLUGIN_DL[(.dll / .so files<br/>in plugins/*/bin/)]
    end

    MAIN --> CFG
    MAIN --> NPL
    MAIN --> SPL
    CFG --> HTTP
    CFG --> STDIO

    HTTP --> PH
    STDIO --> PH

    PH --> RL
    PH --> AK
    PH --> IS
    PH --> SH
    PH --> CR

    CR --> ECHO
    CR --> LLM_CMD
    CR --> SKILL_CMD
    CR --> REMOTE_CMD
    CR --> STA
    CR --> NPA
    CR --> SPA

    LLM_CMD --> LP
    SKILL_CMD --> SE
    SKILL_CMD --> LP
    STA --> SE
    STA --> LP
    SE --> PL
    REMOTE_CMD --> MSR
    REMOTE_CMD --> HC

    NPL --> DLP
    NPL --> WATCH
    DLP --> PLUGIN_DL
    DLP --> NPA
    WATCH -->|new .dll/.so detected| NPL
    NPL -->|notifications/tools/list_changed| STDIO

    SPL --> SPA
    SPA --> SCRIPT

    LP --> LITELLM
    HC --> REMOTE_SRV
```

**Key design decisions:**

| Decision | Rationale |
| -------- | --------- |
| **Unified `CommandRegistry`** | Every tool — built-in, JSON skill, SKILL.md plugin, native DL plugin, script plugin — is a flat `ICommandStrategy`. `tools/list` is a single filtered view. |
| **`skill` meta-tool hidden** | Kept for backward compat but `m_Hidden = true` so it doesn't appear in `tools/list`. Skills are promoted as individual first-class tools. |
| **C ABI for native plugins** | `extern "C"` is the only ABI that is stable across compilers, compiler versions, and standard libraries. Plugin authors compile with any toolchain. |
| **`NativePluginAdapter` fault isolation** | Three protection layers: exception catch → `isError`, 30 s `wait_for` timeout, circuit breaker after 3 faults. A broken plugin cannot crash the host. |
| **Runtime watcher + MCP notification** | The watcher polls `plugins/*/bin/` every 2 s. When a new DL appears it loads, registers tools, and pushes `notifications/tools/list_changed` to the stdio client so LLMs refresh their tool list without reconnecting. |

---

## Request Flow

```mermaid
sequenceDiagram
    participant Client as LLM Client
    participant Transport as HTTP Server / Stdio
    participant PH as Protocol Handler
    participant Sec as Security Layer
    participant CR as Command Registry
    participant Cmd as ICommandStrategy
    participant Ext as External Service

    Client->>Transport: Request (HTTP POST /mcp or stdin JSON-RPC)
    Transport->>PH: Forward request
    PH->>Sec: Rate limit + API key + input sanitization
    Sec-->>PH: OK
    PH->>CR: Dispatch (tools/call or tools/call_batch)
    CR->>Cmd: ExecuteAsync()

    alt Built-in or SkillToolAdapter
        Cmd->>Ext: Call LiteLLM / Remote MCP
        Ext-->>Cmd: Response
    else NativePluginAdapter
        Note over Cmd: exception-isolated<br/>timeout-gated (30 s)<br/>circuit breaker (3 faults)
        Cmd->>Ext: mcp_plugin_execute() via C ABI
        Ext-->>Cmd: JSON result string
    end

    Cmd-->>CR: nlohmann::json result
    CR-->>PH: Result
    PH-->>Transport: JSON response
    Transport-->>Client: HTTP response / stdout JSON-RPC

    Note over Transport,Client: Runtime hot-reload path
    Transport-->>Client: notifications/tools/list_changed<br/>(pushed when watcher loads a new .dll/.so)
```

### Parallel Execution (`tools/call_batch`)

All calls in a batch are launched as `std::async` futures before any `.get()` is called — true within-request parallelism. Total latency ≈ max(individual latencies).

```mermaid
sequenceDiagram
    participant Client
    participant Transport
    participant CR as Command Registry
    participant A as Tool A (async)
    participant B as Tool B (async)

    Client->>Transport: tools/call_batch [{A}, {B}]
    Transport->>CR: HandleToolsCallBatch
    CR->>A: ExecuteAsync() — launch
    CR->>B: ExecuteAsync() — launch
    CR->>A: future.get()
    CR->>B: future.get()
    CR-->>Transport: [{resultA}, {resultB}]
    Transport-->>Client: results array
```

---

## Directory Structure

```text
MCP_Open/
├── include/                  # Public headers
│   ├── commands/             #   CommandRegistry, ICommandStrategy, ToolMetadata
│   │                         #     ToolSource: BuiltIn | JsonSkill | Plugin | NativePlugin | ScriptPlugin
│   ├── core/                 #   ProtocolHandler, Config, Logger, ThreadPool
│   ├── discovery/            #   McpServerRegistry, CompositeCommand
│   ├── http/                 #   IHttpClient
│   ├── llm/                  #   ILLMProvider, LiteLLMProvider, LLMCommand
│   ├── plugins/              #   Plugin systems
│   │   ├── PluginABI.h       #     Stable extern "C" ABI contract (native plugin authors include this)
│   │   ├── IPlugin.h         #     Abstract C++ interface (mockable in tests)
│   │   ├── DlPlugin.h        #     Concrete LoadLibrary/dlopen loader
│   │   ├── NativePluginAdapter.h  # ICommandStrategy wrapper with fault isolation
│   │   ├── NativePluginLoader.h   # Directory scanner, watcher, notify callback
│   │   ├── ScriptPlugin.h    #     POD structs: ScriptPluginToolInfo, ScriptPlugin
│   │   ├── ScriptPluginAdapter.h  # ICommandStrategy wrapper — per-call subprocess spawn
│   │   ├── ScriptPluginLoader.h   # Scans plugin dirs for plugin.json with "runtime" key
│   │   ├── StdioMCPAdapter.h #     ICommandStrategy wrapper for stdio-based MCP child servers
│   │   └── SubprocessPipe.h  #     Cross-platform subprocess I/O (pimpl over subprocess.h)
│   ├── security/             #   RateLimiter, ApiKeyValidator, SecurityHeaders
│   ├── server/               #   IServer, HttplibServer, UwsServer, StdioTransport
│   ├── skills/               #   SkillEngine, SkillCommand, SkillToolAdapter, PluginLoader
│   └── validation/           #   InputSanitizer, JsonSchemaValidator
├── src/                      # Implementation files (mirrors include/)
│   ├── plugins/              #   DlPlugin, NativePluginAdapter, NativePluginLoader,
│   │                         #   ScriptPluginAdapter, ScriptPluginLoader,
│   │                         #   StdioMCPAdapter, SubprocessPipe
│   └── ...
├── plugins/                  # Plugin directory (loaded at runtime) — see plugins/README.md
│   ├── agent-memory/         #   Script plugin — persistent key-value memory store
│   ├── build-tools/          #   Script plugin — build, test, and lint runners
│   ├── desktop-notification/ #   Native plugin — desktop notifications (WinRT/notify-send/osascript)
│   ├── example-plugin/       #   Reference native plugin (ping + base64_encode)
│   ├── filesystem-tools/     #   Script plugin — file system operations (read, write, edit, search)
│   ├── git-tools/            #   Script plugin — git operations (status, changes, sync, conflicts)
│   ├── github-tools/         #   Script plugin — GitHub API integration (PRs, issues, repos)
│   ├── github-actions/       #   Script plugin — GitHub Actions management (workflows, logs, re-runs)
│   ├── shell-tools/          #   Script plugin — shell command execution with timeout
│   ├── unity-tools/          #   Script plugin — Unity3D project info, build, test, asset search, logs
│   ├── unreal-tools/         #   Native plugin — Unreal Engine project info, build, test, assets, logs
│   ├── entrian-search/       #   Skill plugin — Entrian source search
│   ├── everything-search/    #   Skill plugin — Everything file search
│   └── jira-tools/           #   Skill plugin — Jira integration (read-only)
├── skills/                   # JSON skill definitions (loaded at startup)
│   ├── code_review.json
│   ├── summarize.json
│   └── translate.json
├── capi/                     # C API for FFI / P/Invoke
│   ├── include/mcp_capi.h
│   └── src/mcp_capi.cpp
├── csharp/                   # C# wrapper (McpClient.csproj)
├── config/                   # Example configuration files
├── tests/                    # Unit tests (GTest / Catch2) — 202 tests
│   └── test_plugins/         #   Fixture script plugins used by integration tests
│       ├── echo-plugin/      #     echo_tool + fail_tool Python plugin
│       └── stdio-echo/       #     Stdio-based MCP echo server for StdioMCPAdapter tests
├── litellm/                  # LiteLLM proxy launcher & config
├── CMakeLists.txt            # Build system
└── BUILD.md                  # Detailed build instructions
```
