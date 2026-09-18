#pragma once
#include <memory>
#include <string>
#include <vector>

class SubprocessPipe {
public:
    static std::unique_ptr<SubprocessPipe> Spawn(
        const std::string& command, const std::vector<std::string>& args,
        bool combineStderr = false, const std::string& cwd = {});

    bool WriteLine(const std::string& line);
    /// Returns true and fills `line` (without trailing CR/LF) when a line is
    /// available. After the child exits, remaining bytes without a newline are
    /// returned as a final line so fast Windows commands are not dropped.
    bool ReadLine(std::string& line, int timeoutMs);
    bool IsRunning() const;
    void Kill();

    ~SubprocessPipe();

    SubprocessPipe(const SubprocessPipe&) = delete;
    SubprocessPipe& operator=(const SubprocessPipe&) = delete;

private:
    SubprocessPipe();
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
