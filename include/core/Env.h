#pragma once

#include <cstdlib>
#include <string>

/// Portable environment lookup. MSVC treats std::getenv as C4996 under /WX,
/// so all ToolSmith code goes through _dupenv_s there.
inline std::string GetEnvVar(const char* name) {
    if (name == nullptr || name[0] == '\0') return {};
#ifdef _MSC_VER
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) != 0 || val == nullptr) return {};
    std::string result(val);
    std::free(val);
    return result;
#else
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string();
#endif
}
