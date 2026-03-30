#include <collab/process/process.hpp>

#include <cstdlib>
#include <iostream>

int main() {
    // Verify we can create a config and run a basic command
    collab::process::CommandConfig config;
#ifdef _WIN32
    config.program = "cmd";
    config.args = {"/c", "echo", "smoke-test-ok"};
#else
    config.program = "echo";
    config.args = {"smoke-test-ok"};
#endif
    config.stdout_mode = collab::process::CommandConfig::OutputMode::capture;
    config.stderr_mode = collab::process::CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    if (!result) {
        std::cerr << "FAIL: spawn error: " << result.error().what() << "\n";
        return 1;
    }
    if (!result->ok()) {
        std::cerr << "FAIL: exit code " << result->exit_code << "\n";
        return 1;
    }
    if (result->stdout_content.find("smoke-test-ok") == std::string::npos) {
        std::cerr << "FAIL: unexpected output: " << result->stdout_content << "\n";
        return 1;
    }

    std::cout << "PASS: collab-process smoke test\n";
    return 0;
}
