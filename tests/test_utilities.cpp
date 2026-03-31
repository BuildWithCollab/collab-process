#include <catch2/catch_test_macros.hpp>

#include <collab/process/process.hpp>

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
    auto temp = write_temp_file("not an exe", "pe_test");
    REQUIRE(temp.has_value());

    CHECK_FALSE(is_pe_executable(temp.value()));
    fs::remove(temp.value());
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

// ── write_temp_file ────────────────────────────────────────────

TEST_CASE("utilities: write_temp_file creates a file with content", "[utilities]") {
    auto result = write_temp_file("test content here", "wt_test");
    REQUIRE(result.has_value());

    auto path = result.value();
    CHECK(fs::exists(path));

    // Read it back — scope the ifstream so it closes before remove
    {
        std::ifstream in(path);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        CHECK(content == "test content here");
    }

    fs::remove(path);
}

TEST_CASE("utilities: write_temp_file uses the prefix in the filename", "[utilities]") {
    auto result = write_temp_file("x", "myprefix");
    REQUIRE(result.has_value());

    auto filename = result.value().filename().string();
    CHECK(filename.find("myprefix") != std::string::npos);

    fs::remove(result.value());
}

TEST_CASE("utilities: write_temp_file creates unique files", "[utilities]") {
    auto r1 = write_temp_file("one", "uniq");
    auto r2 = write_temp_file("two", "uniq");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());

    CHECK(r1.value() != r2.value());

    fs::remove(r1.value());
    fs::remove(r2.value());
}

TEST_CASE("utilities: write_temp_file with empty content", "[utilities]") {
    auto result = write_temp_file("", "empty");
    REQUIRE(result.has_value());

    CHECK(fs::exists(result.value()));
    CHECK(fs::file_size(result.value()) == 0);

    fs::remove(result.value());
}
