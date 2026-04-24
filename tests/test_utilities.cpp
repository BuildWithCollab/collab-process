#include <catch2/catch_test_macros.hpp>

#include <collab/process.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace collab::process;

// ── find_executable ────────────────────────────────────────────

TEST_CASE("utilities: find_executable finds a known system command", "[utilities]") {
#ifdef _WIN32
    auto result = find_executable("cmd");
#else
    auto result = find_executable("sh");
#endif
    REQUIRE(result.has_value());
    CHECK(fs::exists(result.value()));
}

TEST_CASE("utilities: find_executable returns nullopt for bogus name", "[utilities]") {
    auto result = find_executable("definitely_not_a_real_command_xyz_12345");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("utilities: find_executable does not match a directory", "[utilities]") {
    // Create a temp dir named like a command on PATH
    auto temp_dir = fs::temp_directory_path() / "find_exe_test_dir";
    auto fake_cmd_dir = temp_dir / "fakecmd";
    fs::create_directories(fake_cmd_dir);

    // Prepend our temp dir to PATH so it's searched
    auto old_path = std::getenv("PATH");
    auto new_path = temp_dir.string()
#ifdef _WIN32
        + ";" + (old_path ? old_path : "");
    _putenv_s("PATH", new_path.c_str());
#else
        + ":" + (old_path ? old_path : "");
    setenv("PATH", new_path.c_str(), 1);
#endif

    // "fakecmd" exists on PATH but is a directory — should not match
    auto result = find_executable("fakecmd");
    CHECK_FALSE(result.has_value());

    // Restore PATH and cleanup
#ifdef _WIN32
    _putenv_s("PATH", old_path ? old_path : "");
#else
    setenv("PATH", old_path ? old_path : "", 1);
#endif
    fs::remove_all(temp_dir);
}

TEST_CASE("utilities: find_executable finds test_helper by absolute path", "[utilities]") {
    auto dir = fs::path(TEST_BUILD_DIR);
#ifdef _WIN32
    auto helper = (dir / "test_helper.exe").string();
#else
    auto helper = (dir / "test_helper").string();
#endif

    auto result = find_executable(helper);
    REQUIRE(result.has_value());
    CHECK(fs::exists(result.value()));
}

// ── is_pe_executable ───────────────────────────────────────────

#ifdef _WIN32

TEST_CASE("utilities: is_pe_executable returns true for a real exe", "[utilities][windows]") {
    auto dir = fs::path(TEST_BUILD_DIR);
    auto helper = dir / "test_helper.exe";
    CHECK(is_pe_executable(helper));
}

TEST_CASE("utilities: is_pe_executable returns false for a text file", "[utilities][windows]") {
    auto path = fs::temp_directory_path() / "collab_pe_test.txt";
    {
        std::ofstream f(path, std::ios::binary);
        f << "not an exe";
    }

    CHECK_FALSE(is_pe_executable(path));
    fs::remove(path);
}

TEST_CASE("utilities: is_pe_executable returns false for nonexistent file", "[utilities][windows]") {
    CHECK_FALSE(is_pe_executable("/nonexistent/file.exe"));
}

#else

TEST_CASE("utilities: is_pe_executable always returns false on non-Windows", "[utilities]") {
    auto dir = fs::path(TEST_BUILD_DIR);
    auto helper = dir / "test_helper";
    CHECK_FALSE(is_pe_executable(helper));
}

#endif
