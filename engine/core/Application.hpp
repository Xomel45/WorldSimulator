#pragma once

#include "renderer/Camera.hpp"
#include "renderer/Lights.hpp"
#include "renderer/SceneObject.hpp"
#include "sim/World.hpp"
#include "ui/ImGuiLayer.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace wsim {

class Window;
class VulkanContext;
class InputSystem;
struct Frustum;

struct AppConfig {
    std::string_view title     = "WorldSimulator";
    uint32_t         width     = 1920;
    uint32_t         height    = 1080;
    bool             vsync     = true;
    std::string_view shaderDir;
    std::string_view assetDir;
};

class Application {
public:
    explicit Application(AppConfig config = {});
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    void processInput(float dt);
    void update(float dt);
    void render();
    void renderDebugUI();
    void drawObjects(const std::vector<SceneObject>& objects, const Frustum& frustum);

    AppConfig                      m_config;
    std::unique_ptr<Window>        m_window;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<InputSystem>   m_input;
    std::unique_ptr<ImGuiLayer>    m_imgui;

    OrbitCamera               m_camera;
    std::vector<SceneObject>  m_scene;
    LightsData                m_lights;

    // Симуляция + её визуальное представление. World ничего не знает про
    // Vulkan (см. sim/World.hpp) -- Application только читает population()
    // каждый update() и строит SceneObject'ы из позиций. Все существа делят
    // один и тот же m_creatureMesh (см. деструктор в .cpp -- нельзя удалять
    // меш через перебор m_creatureObjects, иначе двойной destroy()).
    World                     m_world;
    std::shared_ptr<Mesh>     m_creatureMesh;
    std::vector<SceneObject>  m_creatureObjects;

    // Панорамирование/зум — чувствительности god-камеры.
    static constexpr float kPanSpeed    = 12.0f;  // ед./с при distance=1 (масштабируется дистанцией)
    static constexpr float kOrbitSpeed  = 0.2f;   // градусы/пиксель
    static constexpr float kZoomSpeed   = 1.5f;   // ед. дистанции за шаг колеса
};

}  // namespace wsim
