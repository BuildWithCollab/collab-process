#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "collab/process/command_config.hpp"
#include "collab/process/result.hpp"
#include "collab/process/running_process.hpp"

namespace collab::process {

// Fluent builder — sugar over CommandConfig.
// Builds a config internally, delegates to collab::process::run() / spawn().
//
// Temporary chain (deducing this preserves rvalue through the chain):
//   auto result = Command("git").args({"status"}).stdout_capture().run();
//
// Named builder (explicit move to consume):
//   auto cmd = Command("git");
//   cmd.stdout_capture();
//   auto result = std::move(cmd).run();
//
class Command {
    CommandConfig config_;
    IoCallbacks callbacks_;

public:
    explicit Command(std::string program)
        : config_{.program = std::move(program)} {}

    // -- What to run --

    auto arg(this auto&& self, std::string value) -> decltype(auto) {
        self.config_.args.push_back(std::move(value));
        return std::forward<decltype(self)>(self);
    }

    auto args(this auto&& self, std::initializer_list<std::string_view> values) -> decltype(auto) {
        for (auto v : values)
            self.config_.args.emplace_back(v);
        return std::forward<decltype(self)>(self);
    }

    auto args(this auto&& self, std::vector<std::string> values) -> decltype(auto) {
        for (auto& v : values)
            self.config_.args.push_back(std::move(v));
        return std::forward<decltype(self)>(self);
    }

    // -- Where to run --

    auto working_directory(this auto&& self, std::filesystem::path path) -> decltype(auto) {
        self.config_.working_dir = std::move(path);
        return std::forward<decltype(self)>(self);
    }

    // -- Environment --

    auto env(this auto&& self, std::string key, std::string value) -> decltype(auto) {
        self.config_.env_add.emplace_back(std::move(key), std::move(value));
        return std::forward<decltype(self)>(self);
    }

    auto env_remove(this auto&& self, std::string key) -> decltype(auto) {
        self.config_.env_remove.push_back(std::move(key));
        return std::forward<decltype(self)>(self);
    }

    auto env_clear(this auto&& self) -> decltype(auto) {
        self.config_.env_clear = true;
        return std::forward<decltype(self)>(self);
    }

    // -- Stdin --

    auto stdin_string(this auto&& self, std::string content) -> decltype(auto) {
        self.config_.stdin_content = std::move(content);
        return std::forward<decltype(self)>(self);
    }

    auto stdin_file(this auto&& self, std::filesystem::path path) -> decltype(auto) {
        self.config_.stdin_path = std::move(path);
        return std::forward<decltype(self)>(self);
    }

    auto stdin_close(this auto&& self) -> decltype(auto) {
        self.config_.stdin_closed = true;
        return std::forward<decltype(self)>(self);
    }

    auto stdin_inherit(this auto&& self) -> decltype(auto) {
        // Default — clear any previously set stdin options
        self.config_.stdin_content.clear();
        self.config_.stdin_path.clear();
        self.config_.stdin_closed = false;
        return std::forward<decltype(self)>(self);
    }

    // -- Stdout --

    auto stdout_capture(this auto&& self) -> decltype(auto) {
        self.config_.stdout_mode = CommandConfig::OutputMode::capture;
        return std::forward<decltype(self)>(self);
    }

    auto stdout_inherit(this auto&& self) -> decltype(auto) {
        self.config_.stdout_mode = CommandConfig::OutputMode::inherit;
        return std::forward<decltype(self)>(self);
    }

    auto stdout_discard(this auto&& self) -> decltype(auto) {
        self.config_.stdout_mode = CommandConfig::OutputMode::discard;
        return std::forward<decltype(self)>(self);
    }

    auto stdout_callback(this auto&& self,
        collab::process::move_only_function<void(std::string_view)> cb) -> decltype(auto) {
        self.callbacks_.on_stdout = std::move(cb);
        return std::forward<decltype(self)>(self);
    }

    // -- Stderr --

    auto stderr_capture(this auto&& self) -> decltype(auto) {
        self.config_.stderr_mode = CommandConfig::OutputMode::capture;
        return std::forward<decltype(self)>(self);
    }

    auto stderr_inherit(this auto&& self) -> decltype(auto) {
        self.config_.stderr_mode = CommandConfig::OutputMode::inherit;
        return std::forward<decltype(self)>(self);
    }

    auto stderr_discard(this auto&& self) -> decltype(auto) {
        self.config_.stderr_mode = CommandConfig::OutputMode::discard;
        return std::forward<decltype(self)>(self);
    }

    auto stderr_merge(this auto&& self) -> decltype(auto) {
        self.config_.stderr_merge = true;
        return std::forward<decltype(self)>(self);
    }

    auto stderr_callback(this auto&& self,
        collab::process::move_only_function<void(std::string_view)> cb) -> decltype(auto) {
        self.callbacks_.on_stderr = std::move(cb);
        return std::forward<decltype(self)>(self);
    }

    // -- Behavior --

    auto timeout(this auto&& self, std::chrono::milliseconds ms) -> decltype(auto) {
        self.config_.timeout = ms;
        return std::forward<decltype(self)>(self);
    }

    auto detached(this auto&& self) -> decltype(auto) {
        self.config_.detached = true;
        return std::forward<decltype(self)>(self);
    }

    // -- Execute (consumes the Command) --

    auto run(this Command&& self) -> std::expected<Result, SpawnError>;
    auto spawn(this Command&& self) -> std::expected<RunningProcess, SpawnError>;
};

}  // namespace collab::process
