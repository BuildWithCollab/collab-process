#include "env_block.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
extern "C" char** environ;
#endif

namespace collab::process::detail {

auto build_env_block(const CommandConfig& config) -> std::vector<std::string> {
    std::vector<std::string> entries;

    // Step 1: start from parent env (unless env_clear)
    if (!config.env_clear) {
#ifdef _WIN32
        auto* env_strings = GetEnvironmentStringsA();
        if (env_strings) {
            const char* p = env_strings;
            while (*p) {
                std::string entry(p);
                entries.push_back(entry);
                p += entry.size() + 1;
            }
            FreeEnvironmentStringsA(env_strings);
        }
#else
        for (char** e = ::environ; *e; ++e) {
            entries.emplace_back(*e);
        }
#endif
    }

    // Step 2: remove requested vars
    for (auto& key : config.env_remove) {
        auto prefix = key + "=";
        std::erase_if(entries, [&](const std::string& entry) {
            return entry.starts_with(prefix);
        });
    }

    // Step 3: add/override requested vars
    for (auto& [key, value] : config.env_add) {
        auto prefix = key + "=";
        // Remove existing entry with same key first
        std::erase_if(entries, [&](const std::string& entry) {
            return entry.starts_with(prefix);
        });
        entries.push_back(key + "=" + value);
    }

    return entries;
}

}  // namespace collab::process::detail
