#pragma once
#include <memory>
#include <string>
#include <vector>

class SubprocessPipe {
public:
    static std::unique_ptr<SubprocessPipe> Spawn(
        const std::string& command, const std::vector<std::string>& args,
        bool combineStderr = false);

    bool WriteLine(const std::string& line);
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
