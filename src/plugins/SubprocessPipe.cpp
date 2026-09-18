#include <plugins/SubprocessPipe.h>
#include <core/Logger.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifdef _MSC_VER
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <subprocess.h>

#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct SubprocessPipe::Impl {
    subprocess_s process{};
    std::string readBuffer;
    bool destroyed = false;
};

SubprocessPipe::SubprocessPipe() : m_Impl(std::make_unique<Impl>()) {}

SubprocessPipe::~SubprocessPipe() {
    if (m_Impl && !m_Impl->destroyed) {
        if (subprocess_alive(&m_Impl->process))
            subprocess_terminate(&m_Impl->process);
        subprocess_destroy(&m_Impl->process);
        m_Impl->destroyed = true;
    }
}

std::unique_ptr<SubprocessPipe> SubprocessPipe::Spawn(
    const std::string& command, const std::vector<std::string>& args,
    bool combineStderr, const std::string& cwd)
{
    std::vector<const char*> cmdLine;
    cmdLine.push_back(command.c_str());
    for (const auto& a : args)
        cmdLine.push_back(a.c_str());
    cmdLine.push_back(nullptr);

    int options = subprocess_option_search_user_path
                | subprocess_option_enable_async
                | subprocess_option_enable_async_no_wait
                | subprocess_option_inherit_environment;
    if (combineStderr) {
        options |= subprocess_option_combined_stdout_stderr;
    }
#ifdef _WIN32
    options |= subprocess_option_no_window;
#endif

    auto pipe = std::unique_ptr<SubprocessPipe>(new SubprocessPipe());
    const char* cwdPtr = cwd.empty() ? nullptr : cwd.c_str();
    int ret = subprocess_create_ex(cmdLine.data(), options, nullptr, cwdPtr,
                                   &pipe->m_Impl->process);
    if (ret != 0) {
        throw std::runtime_error(
            "SubprocessPipe: failed to create process: " + command);
    }

    return pipe;
}

bool SubprocessPipe::WriteLine(const std::string& line) {
    FILE* in = subprocess_stdin(&m_Impl->process);
    if (!in) return false;

    std::string data = line + "\n";
    size_t written = fwrite(data.c_str(), 1, data.size(), in);
    fflush(in);
    return written == data.size();
}

bool SubprocessPipe::ReadLine(std::string& line, int timeoutMs) {
    line.clear();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeoutMs);

    auto takeLine = [this, &line]() -> bool {
        auto pos = m_Impl->readBuffer.find('\n');
        if (pos == std::string::npos) return false;
        line = m_Impl->readBuffer.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        m_Impl->readBuffer.erase(0, pos + 1);
        return true;
    };

    auto takeRemainder = [this, &line]() -> bool {
        if (m_Impl->readBuffer.empty()) return false;
        line = m_Impl->readBuffer;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        m_Impl->readBuffer.clear();
        return true;
    };

    auto appendStdout = [this]() -> unsigned {
        char buf[4096];
        unsigned n = subprocess_read_stdout(&m_Impl->process, buf, sizeof(buf));
        if (n > 0) m_Impl->readBuffer.append(buf, n);
        return n;
    };

    while (std::chrono::steady_clock::now() < deadline) {
        if (takeLine()) return true;

        unsigned n = appendStdout();
        if (n > 0) continue;

        if (!subprocess_alive(&m_Impl->process)) {
            // PeekNamedPipe in no_wait mode returns 0 after the child exits
            // even when bytes remain. One blocking drain recovers them.
            m_Impl->process.no_wait = 0;
            while (appendStdout() > 0) {}
            if (takeLine()) return true;
            return takeRemainder();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool SubprocessPipe::IsRunning() const {
    if (!m_Impl || m_Impl->destroyed) return false;
    return subprocess_alive(&m_Impl->process) != 0;
}

void SubprocessPipe::Kill() {
    if (m_Impl && !m_Impl->destroyed && subprocess_alive(&m_Impl->process))
        subprocess_terminate(&m_Impl->process);
}
