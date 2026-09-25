#include "maya/assets/project.hpp"
#include "maya/assets/registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
/// A scratch folder, removed afterwards.
struct Folder {
    fs::path root = fs::temp_directory_path() /
        ("maya-project-" + std::to_string(::getpid()) + "-" + std::to_string(detail::next_lifetime_token()));
    Folder() { fs::create_directories(root); }
    ~Folder() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void write(const fs::path& path, const std::string& text) const {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << text;
    }
};
ProjectSettingsResult parse(const std::string& text) {
    auto input = std::istringstream(text);
    return read_project(input);
}
/// Restores the working directory when it goes out of scope.
struct WorkingDirectory {
    fs::path previous = fs::current_path();
    explicit WorkingDirectory(const fs::path& path) { fs::current_path(path); }
    ~WorkingDirectory() { fs::current_path(previous); }
};
} // namespace

TEST_CASE("Project settings round-trip and reject paths that would not move with the project", "[assets][project]") {
    const auto parsed = parse("maya-project 1\ncontent \"assets\"\ncatalog \"catalog.maya\"\nstartup \"levels/intro.scene\"\n");
    REQUIRE(parsed);
    CHECK(parsed.settings.content == "assets");
    CHECK(parsed.settings.catalog == "catalog.maya");
    CHECK(parsed.settings.startup_scene == "levels/intro.scene");
    auto written = std::ostringstream{};
    write_project(written, parsed.settings);
    CHECK(written.str() == "maya-project 1\ncontent \"assets\"\ncatalog \"catalog.maya\"\nstartup \"levels/intro.scene\"\n");

    // The startup scene is optional; quoted paths may hold spaces.
    const auto minimal = parse("maya-project 1\ncontent \".\"\ncatalog \"My Assets/catalog.maya\"\n");
    REQUIRE(minimal);
    CHECK(minimal.settings.content == ".");
    CHECK(minimal.settings.catalog == "My Assets/catalog.maya");
    CHECK(minimal.settings.startup_scene.empty());

    const auto rejected = [](const std::string& text, const std::string& reason) {
        const auto result = parse(text);
        INFO(text);
        CHECK_FALSE(result);
        CHECK(result.error.find(reason) != std::string::npos);
    };
    rejected("maya-assets 1\n", "maya-project header");
    rejected("maya-project 2\ncontent \".\"\ncatalog \"c.maya\"\n", "Unsupported project version 2");
    rejected("maya-project 1\ncatalog \"c.maya\"\n", "Expected 'content'");
    rejected("maya-project 1\ncontent \".\"\n", "Missing 'catalog'");
    rejected("maya-project 1\ncontent \"/Users/someone/game\"\ncatalog \"c.maya\"\n", "relative path inside the project");
    rejected("maya-project 1\ncontent \"../shared\"\ncatalog \"c.maya\"\n", "relative path inside the project");
    rejected("maya-project 1\ncontent \".\"\ncatalog \"../c.maya\"\n", "relative file path inside the content root");
    rejected("maya-project 1\ncontent \".\"\ncatalog \".\"\n", "relative file path inside the content root");
    rejected("maya-project 1\ncontent \".\"\ncatalog \"c.maya\"\nstartup \"/tmp/a.scene\"\n", "startup scene");
    rejected("maya-project 1\ncontent \".\"\ncatalog \"c.maya\"\nstartup \"a.scene\"\nextra 1\n", "Unexpected 'extra'");
    rejected("maya-project 1\ncontent \".\"\ncatalog \"c.maya\"\nsettings\n", "Unexpected 'settings'");
}

TEST_CASE("Opening a project finds its content root from its file, wherever it is", "[assets][project]") {
    const auto folder = Folder{};
    folder.write("game/project.maya", "maya-project 1\ncontent \"content\"\ncatalog \"catalog.maya\"\nstartup \"main.scene\"\n");
    folder.write("game/content/catalog.maya", "maya-assets 1\n");
    const auto root = fs::canonical(folder.root);

    SECTION("From the file or its folder") {
        for (const auto& path : {folder.root / "game/project.maya", folder.root / "game"}) {
            const auto opened = open_project(path);
            REQUIRE(opened);
            CHECK(opened.project.file == root / "game/project.maya");
            CHECK(opened.project.content_root == root / "game/content");
            CHECK(opened.project.catalog == root / "game/content/catalog.maya");
            CHECK(opened.project.startup_scene == root / "game/content/main.scene"); // need not exist yet
            CHECK(opened.project.name() == "game");
        }
    }
    SECTION("A relative path is taken from the working directory, and nothing else depends on it") {
        auto opened = ProjectResult{};
        {
            const auto cwd = WorkingDirectory(folder.root);
            opened = open_project("game");
            REQUIRE(opened);
        }
        CHECK(opened.project.content_root == root / "game/content");
        // A copy elsewhere resolves to its own content.
        fs::copy(folder.root / "game", folder.root / "copy", fs::copy_options::recursive);
        const auto cwd = WorkingDirectory(folder.root / "game/content");
        const auto copy = open_project("../../copy/project.maya");
        REQUIRE(copy);
        CHECK(copy.project.content_root == root / "copy/content");
    }
    SECTION("Paths resolve inside the content root only") {
        const auto project = open_project(folder.root / "game").project;
        CHECK(project.resolve("levels/one.scene") == root / "game/content/levels/one.scene");
        CHECK(project.resolve("levels/../one.scene") == root / "game/content/one.scene");
        CHECK(project.resolve(root / "game/content/two.scene") == root / "game/content/two.scene");
        CHECK_FALSE(project.resolve("../project.maya"));
        CHECK_FALSE(project.resolve(root / "game/project.maya"));
        CHECK_FALSE(project.resolve(""));
        CHECK_FALSE(project.resolve("."));
        CHECK(project.relative(root / "game/content/levels/one.scene") == "levels/one.scene");
        CHECK(project.relative(root / "game/project.maya").empty());
        // A symlink that leaves the content root is outside it.
        fs::create_directory_symlink(folder.root, folder.root / "game/content/escape");
        CHECK_FALSE(project.resolve("escape/game/project.maya"));
    }
    SECTION("Problems name the file and the reason") {
        folder.write("broken/project.maya", "maya-project 1\ncontent \"missing\"\ncatalog \"catalog.maya\"\n");
        auto opened = open_project(folder.root / "broken");
        CHECK_FALSE(opened);
        CHECK(opened.error.find("content root") != std::string::npos);
        CHECK(opened.error.find("is not a directory") != std::string::npos);
        opened = open_project(folder.root / "nowhere");
        CHECK(opened.error.find("Cannot read project file") != std::string::npos);
        folder.write("bad/project.maya", "maya-project 1\ncontent \"..\"\ncatalog \"catalog.maya\"\n");
        opened = open_project(folder.root / "bad/project.maya");
        CHECK(opened.error.find("project.maya: The content root") != std::string::npos);
        folder.write("file/project.maya", "maya-project 1\ncontent \"catalog.maya\"\ncatalog \"catalog.maya\"\n");
        folder.write("file/catalog.maya", "maya-assets 1\n");
        CHECK_FALSE(open_project(folder.root / "file"));
    }
}
