#include "core/Application.hpp"

#include "core/InputSystem.hpp"
#include "core/Logger.hpp"
#include "renderer/Frustum.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/Window.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <utility>

namespace wsim {

namespace {

// Плоская земляная плита (пока не процедурная — просто чтобы было на что
// смотреть и на чём проверять god-камеру). Границы берём из World —
// единственный источник истины для размера мира (раньше здесь было
// отдельное захардкоженное число).
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildGroundPlane() {
    const glm::vec4 normal{0.f, 1.f, 0.f, 0.f};
    const glm::vec4 color{0.24f, 0.32f, 0.22f, 1.f};
    const float      h = World::kWorldHalfSize;

    std::vector<Vertex> verts = {
        {{-h, 0.f, -h, 0.f}, normal, color},
        {{ h, 0.f, -h, 0.f}, normal, color},
        {{ h, 0.f,  h, 0.f}, normal, color},
        {{-h, 0.f,  h, 0.f}, normal, color},
    };
    std::vector<uint32_t> indices = {0, 1, 2, 2, 3, 0};
    return {std::move(verts), std::move(indices)};
}

// Маркер существа: маленькая пирамида (квадратное основание + смещённая
// вперёд/вверх вершина), чтобы форма сама по себе читалась как "смотрит
// вперёд" в паре с Transform::rotation.y (см. Application::update()).
// Нормали общие на вершину, как и у buildGroundPlane() -- освещение пока
// только ambient (см. m_lights ниже), точность нормалей по граням не видна.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildCreatureMesh() {
    const glm::vec4 normal{0.f, 1.f, 0.f, 0.f};
    const glm::vec4 color{0.85f, 0.7f, 0.3f, 1.f};
    constexpr float halfBase = 0.4f;
    constexpr float apexY    = 0.8f;
    constexpr float apexZ    = 0.3f;  // смещение вершины вперёд по +Z

    std::vector<Vertex> verts = {
        {{-halfBase, 0.f, -halfBase, 0.f}, normal, color},  // 0
        {{ halfBase, 0.f, -halfBase, 0.f}, normal, color},  // 1
        {{ halfBase, 0.f,  halfBase, 0.f}, normal, color},  // 2
        {{-halfBase, 0.f,  halfBase, 0.f}, normal, color},  // 3
        {{      0.f, apexY,    apexZ, 0.f}, normal, color},  // 4 -- вершина
    };
    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,  // основание
        0, 1, 4,           // бока
        1, 2, 4,
        2, 3, 4,
        3, 0, 4,
    };
    return {std::move(verts), std::move(indices)};
}

}  // namespace

// ─── Constructor ──────────────────────────────────────────────────────────────
Application::Application(AppConfig config)
    : m_config(config), m_world(std::random_device{}()) {
    Logger::info("Initializing WorldSimulator core");

    m_window  = std::make_unique<Window>(config.title, config.width, config.height);
    m_context = std::make_unique<VulkanContext>(*m_window, config.vsync, std::string(config.shaderDir));
    m_context->setMsaa(VK_SAMPLE_COUNT_4_BIT);
    m_input   = std::make_unique<InputSystem>(m_window->handle());
    m_imgui   = std::make_unique<ImGuiLayer>(m_window->handle(), *m_context, std::string(config.assetDir));

    // Земляная плоскость — статичный объект сцены.
    auto [groundV, groundI] = buildGroundPlane();
    auto groundMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        groundV, groundI));
    m_scene.push_back({groundMesh, Transform{}, /*solid=*/false});

    // Один общий меш на всех существ — SceneObject'ы (см. update()) просто
    // ссылаются на него с разным Transform, ни одной лишней GPU-аллокации.
    auto [creatureV, creatureI] = buildCreatureMesh();
    m_creatureMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        creatureV, creatureI));

    // Освещение: только ambient — direct/point-света пока не заведены (см. CLAUDE.md).
    m_lights.ambient = {1.0f, 1.0f, 1.0f, 0.6f};

    Logger::info("World: {} creatures, {} food sources", m_world.populationCount(), m_world.foodCount());
    Logger::info("Engine ready");
}

Application::~Application() {
    Logger::info("Shutting down");
    if (m_context) {
        vkDeviceWaitIdle(m_context->device());
        for (auto& obj : m_scene)
            if (obj.mesh) obj.mesh->destroy(m_context->device());
        // m_creatureObjects делят один и тот же m_creatureMesh -- удаляем
        // его РОВНО один раз, а не через перебор объектов сцены (иначе
        // Mesh::destroy() вызвался бы на одних и тех же GPU-буферах
        // многократно -- double free).
        if (m_creatureMesh) m_creatureMesh->destroy(m_context->device());
    }
}

// ─── run ──────────────────────────────────────────────────────────────────────
void Application::run() {
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::duration<float>;

    constexpr float kFixedStep = 1.0f / 60.0f;
    constexpr float kMaxDelta  = 0.25f;
    float           accumulator = 0.0f;
    auto            lastTime    = Clock::now();

    Logger::info("Entering game loop");

    // WSIM_AUTOEXIT=1 — авто-выход через 5 с (смоук-тест полного цикла с деструкторами).
    const bool autoExit  = std::getenv("WSIM_AUTOEXIT") != nullptr;
    float      totalTime = 0.f;

    while (!m_window->shouldClose()) {
        auto  now   = Clock::now();
        float delta = Duration(now - lastTime).count();
        lastTime    = now;
        if (delta > kMaxDelta) delta = kMaxDelta;
        accumulator += delta;

        m_window->pollEvents();
        processInput(delta);

        while (accumulator >= kFixedStep) {
            update(kFixedStep);
            accumulator -= kFixedStep;
        }

        render();

        totalTime += delta;
        if (autoExit && totalTime > 5.f) glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);
    }
}

// ─── processInput ─────────────────────────────────────────────────────────────
void Application::processInput(float dt) {
    m_input->update();

    // Панорамирование: WASD, масштабируется дистанцией (быстрее при отдалении).
    const float panAmount = kPanSpeed * m_camera.distance() / 25.0f * dt;
    float forward = 0.f, right = 0.f;
    if (m_input->isKeyDown(GLFW_KEY_W)) forward += panAmount;
    if (m_input->isKeyDown(GLFW_KEY_S)) forward -= panAmount;
    if (m_input->isKeyDown(GLFW_KEY_D)) right   += panAmount;
    if (m_input->isKeyDown(GLFW_KEY_A)) right   -= panAmount;
    if (forward != 0.f || right != 0.f) m_camera.pan(forward, right);

    // Вращение: зажатая ПКМ + движение мыши.
    if (m_input->isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        m_camera.orbit(m_input->mouseDX() * kOrbitSpeed, -m_input->mouseDY() * kOrbitSpeed);
    }

    // Зум колесом.
    const float scroll = m_input->scrollDY();
    if (scroll != 0.f) m_camera.zoom(-scroll * kZoomSpeed);

    if (m_input->isKeyPressed(GLFW_KEY_F10)) glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);
}

// ─── update ───────────────────────────────────────────────────────────────────
void Application::update(float dt) {
    m_world.tick(dt);

    // Пересобираем визуальные объекты существ из текущей популяции -- дёшево:
    // SceneObject это shared_ptr на уже созданный меш + Transform, никакой
    // GPU-работы. Разворот (yaw) берём из направления движения; если
    // существо стоит на месте, ориентацию не трогаем (у Creature нет своего
    // hеading-поля — приемлемое упрощение для первой версии, см. CLAUDE.md).
    m_creatureObjects.clear();
    m_creatureObjects.reserve(m_world.populationCount());
    for (const auto& inhabitant : m_world.population()) {
        const Creature& c = inhabitant.creature;
        Transform        t;
        t.position = c.position;
        t.scale    = glm::vec3(c.size());
        if (const float speed = glm::length(c.velocity); speed > 0.01f)
            t.rotation.y = glm::degrees(std::atan2(c.velocity.x, c.velocity.z));
        m_creatureObjects.push_back({m_creatureMesh, t, /*solid=*/false});
    }
}

// ─── render ───────────────────────────────────────────────────────────────────
void Application::render() {
    m_imgui->newFrame();
    renderDebugUI();

    m_context->beginFrame();
    if (m_context->isFrameValid()) {
        const VkExtent2D ext = m_context->swapchainExtent();
        m_camera.setAspect(static_cast<float>(ext.width) / static_cast<float>(ext.height));

        const CameraData camData = m_camera.getData();
        m_context->updateCamera(camData);
        m_context->updateLights(m_lights);

        const Frustum frustum = Frustum::fromMatrix(camData.proj * camData.view);

        drawObjects(m_scene, frustum);
        drawObjects(m_creatureObjects, frustum);

        m_context->beginUIPass();
        m_imgui->renderDrawData(m_context->currentCommandBuffer());
    }
    m_context->endFrame();
}

// ─── drawObjects ──────────────────────────────────────────────────────────────
// Общий по фрустум-отсечению + drawMesh цикл — раньше жил только для m_scene,
// теперь его же используем для m_creatureObjects (см. render()).
void Application::drawObjects(const std::vector<SceneObject>& objects, const Frustum& frustum) {
    for (const auto& obj : objects) {
        if (!obj.mesh) continue;

        const glm::mat4 model = obj.transform.matrix();
        if (!frustum.intersects(obj.mesh->localAABB.transformed(model))) continue;

        const VulkanContext::MeshPC pc{
            .model        = model,
            .normalMatrix = glm::transpose(glm::inverse(model)),
        };

        m_context->drawMesh(obj.mesh->vbo.handle, obj.mesh->ibo.handle, obj.mesh->indexCount, pc);
    }
}

// ─── renderDebugUI ─────────────────────────────────────────────────────────────
void Application::renderDebugUI() {
    ImGui::SetNextWindowPos({20.f, 20.f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("WorldSimulator — core");
    ImGui::Text("FPS: %.0f (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Separator();
    const glm::vec3 t = m_camera.target();
    ImGui::Text("Target:   %.1f, %.1f, %.1f", t.x, t.y, t.z);
    ImGui::Text("Yaw/Pitch: %.1f / %.1f", m_camera.yaw(), m_camera.pitch());
    ImGui::Text("Distance: %.1f", m_camera.distance());
    ImGui::Separator();
    ImGui::Text("Population: %zu", m_world.populationCount());
    ImGui::Text("Food sources: %zu", m_world.foodCount());
    ImGui::Separator();
    ImGui::TextUnformatted("WASD pan, RMB-drag orbit, wheel zoom, F10 quit");
    ImGui::End();
}

}  // namespace wsim
