#include "core/Application.hpp"
#include "core/Logger.hpp"
#include "platform/Paths.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace {
// Переносимая раскладка (релиз): рядом с исполняемым файлом лежат shaders/, assets/ и mods/.
// Иначе (запуск из дерева сборки) берутся пути, зашитые при конфигурации.
struct Dirs {
    std::string shaders = WSIM_SHADER_DIR, assets = WSIM_ASSET_DIR, mods = WSIM_MODS_DIR;
    Dirs() {
        namespace fs = std::filesystem;
        const fs::path root = wsim::paths::exeDir();
        if (!root.empty() && fs::is_directory(root / "assets" / "fonts") && fs::is_directory(root / "shaders")) {
            auto dir = [](const fs::path& p) { return (p / "").string(); };  // с завершающим разделителем
            shaders = dir(root / "shaders");
            assets  = dir(root / "assets");
            mods    = dir(root / "mods");
        }
    }
};
}  // namespace

int main() {
    try {
        const Dirs dirs;
        wsim::Application app({
            .title     = "WorldSimulator",
            .width     = 1920,
            .height    = 1080,
            .vsync     = true,
            .shaderDir = dirs.shaders,
            .assetDir  = dirs.assets,
            .modsDir   = dirs.mods,
        });
        app.run();
    } catch (const std::exception& e) {
        wsim::Logger::error("Fatal: {}", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
