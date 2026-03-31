#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

using namespace collab::process;

TEST_CASE("smoke: captures stdout from a command", "[smoke]") {
    CommandConfig config;
#ifdef _WIN32
    config.program = "cmd";
    config.args = {"/c", "echo", "smoke-test-ok"};
#else
    config.program = "echo";
    config.args = {"smoke-test-ok"};
#endif
    config.stdout_mode = CommandConfig::OutputMode::capture;
    config.stderr_mode = CommandConfig::OutputMode::discard;

    auto result = collab::process::run(config);
    REQUIRE(result.has_value());
    CHECK(result->ok());
    CHECK(result->stdout_content.find("smoke-test-ok") != std::string::npos);
}
