#include "core/Application.hpp"
#include "core/Logger.hpp"

#include <cstdlib>
#include <exception>

int main() {
    try {
        wsim::Application app({
            .title     = "WorldSimulator",
            .width     = 1920,
            .height    = 1080,
            .vsync     = true,
            .shaderDir = WSIM_SHADER_DIR,
            .assetDir  = WSIM_ASSET_DIR,
        });
        app.run();
    } catch (const std::exception& e) {
        wsim::Logger::error("Fatal: {}", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
