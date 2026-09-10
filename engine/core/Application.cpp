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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <utility>

namespace wsim {

namespace {

// Разрешение сетки рельефа: ~2 юнита на ячейку -> ~101x101 вершин на весь
// мир (World::kWorldHalfSize=100), ~20k треугольников -- тривиально для GPU,
// строится один раз в конструкторе.
constexpr float kTerrainCellSize = 2.0f;

// Рельеф: реальная геометрия (Terrain::heightAt() на каждую вершину), а не
// покраска плоскости -- см. обсуждение в CLAUDE.md/чате. Нормали считаются
// из самой сгенерированной сетки высот (центральные разности, вперёд/назад
// на краях) -- в отличие от buildCreatureMesh()'s общей нормали, здесь это
// важно: холмы должны реально читаться под directional-светом (см. m_lights
// ниже), а не просто существовать геометрически.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildTerrainMesh(const Terrain& terrain) {
    const float halfSize = terrain.halfSize();
    const int   res       = static_cast<int>(std::round(2.0f * halfSize / kTerrainCellSize)) + 1;

    const auto idx  = [res](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(res) + static_cast<std::size_t>(i); };
    const auto xAt  = [halfSize](int i) { return -halfSize + static_cast<float>(i) * kTerrainCellSize; };
    const auto zAt  = [halfSize](int j) { return -halfSize + static_cast<float>(j) * kTerrainCellSize; };

    // Проход 1: высоты сетки + диапазон (нужен для нормализованной покраски
    // ниже -- реальный диапазон зависит от сида, фиксированные константы
    // давали бы приглушённый/невыразительный градиент на большинстве сидов).
    std::vector<float> heights(static_cast<std::size_t>(res) * static_cast<std::size_t>(res));
    float minH = std::numeric_limits<float>::max();
    float maxH = std::numeric_limits<float>::lowest();
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const float h  = terrain.heightAt(xAt(i), zAt(j));
            heights[idx(i, j)] = h;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    const float heightRange = std::max(maxH - minH, 0.001f);

    const glm::vec4 lowColor{0.24f, 0.32f, 0.22f, 1.f};   // низина -- трава (тот же цвет, что был у старой плоской плиты)
    const glm::vec4 highColor{0.55f, 0.52f, 0.45f, 1.f};  // высота -- камень/сушь

    std::vector<Vertex> verts;
    verts.reserve(static_cast<std::size_t>(res) * static_cast<std::size_t>(res));
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const int iL = std::max(i - 1, 0), iR = std::min(i + 1, res - 1);
            const int jD = std::max(j - 1, 0), jU = std::min(j + 1, res - 1);
            const float dx = (heights[idx(iR, j)] - heights[idx(iL, j)]) /
                              (static_cast<float>(iR - iL) * kTerrainCellSize);
            const float dz = (heights[idx(i, jU)] - heights[idx(i, jD)]) /
                              (static_cast<float>(jU - jD) * kTerrainCellSize);
            const glm::vec3 normal = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
            const glm::vec4 normal4{normal, 0.f};

            const float      h = heights[idx(i, j)];
            const float      t = std::clamp((h - minH) / heightRange, 0.0f, 1.0f);
            const glm::vec4  color = glm::mix(lowColor, highColor, t);

            verts.push_back({{xAt(i), h, zAt(j), 0.f}, normal4, color});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(res - 1) * static_cast<std::size_t>(res - 1) * 6);
    for (int j = 0; j < res - 1; ++j) {
        for (int i = 0; i < res - 1; ++i) {
            const auto a = static_cast<uint32_t>(idx(i, j));
            const auto b = static_cast<uint32_t>(idx(i + 1, j));
            const auto c = static_cast<uint32_t>(idx(i + 1, j + 1));
            const auto d = static_cast<uint32_t>(idx(i, j + 1));
            indices.insert(indices.end(), {a, b, c, c, d, a});
        }
    }
    return {std::move(verts), std::move(indices)};
}

// Плоская вода на уровне моря, во весь мир -- глубина/берега появляются
// сами по себе через depth-test против рельефа (суша торчит и перекрывает
// воду, впадины прячутся под ней), без отдельной логики. Непрозрачная: в
// scene-пайплайне blending не включён (fragColor.a уже занят как флаг
// "это бетон", не альфа) -- см. CLAUDE.md, настоящая прозрачность отложена.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildWaterPlane(float halfSize) {
    const glm::vec4 normal{0.f, 1.f, 0.f, 0.f};
    const glm::vec4 color{0.16f, 0.35f, 0.55f, 1.f};
    const float      h = halfSize;
    const float      y = Terrain::kSeaLevel;

    std::vector<Vertex> verts = {
        {{-h, y, -h, 0.f}, normal, color},
        {{ h, y, -h, 0.f}, normal, color},
        {{ h, y,  h, 0.f}, normal, color},
        {{-h, y,  h, 0.f}, normal, color},
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
    : m_config(config),
      m_world(std::random_device{}()),
      m_terrain(World::kWorldHalfSize, std::random_device{}()) {
    Logger::info("Initializing WorldSimulator core");

    m_window  = std::make_unique<Window>(config.title, config.width, config.height);
    m_context = std::make_unique<VulkanContext>(*m_window, config.vsync, std::string(config.shaderDir));
    m_context->setMsaa(VK_SAMPLE_COUNT_4_BIT);
    m_input   = std::make_unique<InputSystem>(m_window->handle());
    m_imgui   = std::make_unique<ImGuiLayer>(m_window->handle(), *m_context, std::string(config.assetDir));

    // Рельеф + вода — статичные объекты сцены, строятся один раз.
    auto [terrainV, terrainI] = buildTerrainMesh(m_terrain);
    auto terrainMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        terrainV, terrainI));
    m_scene.push_back({terrainMesh, Transform{}, /*solid=*/false});

    auto [waterV, waterI] = buildWaterPlane(m_terrain.halfSize());
    auto waterMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        waterV, waterI));
    m_scene.push_back({waterMesh, Transform{}, /*solid=*/false});

    // Один общий меш на всех существ — SceneObject'ы (см. update()) просто
    // ссылаются на него с разным Transform, ни одной лишней GPU-аллокации.
    auto [creatureV, creatureI] = buildCreatureMesh();
    m_creatureMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        creatureV, creatureI));

    // Освещение: ambient + солнце (направленный свет — иначе рельеф не
    // читается как рельеф, только силуэтом, см. CLAUDE.md). Фиксированный
    // угол, без дня/ночи (вне охвата этого раунда).
    m_lights.ambient          = {1.0f, 1.0f, 1.0f, 0.6f};
    m_lights.sunDir            = glm::normalize(glm::vec4(0.4f, 1.0f, 0.3f, 0.0f));
    m_lights.sunColorIntensity = {1.0f, 0.98f, 0.92f, 0.9f};

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
        // c.position.y всегда 0 (симуляция не знает о рельефе в этом раунде,
        // см. CLAUDE.md) -- сэмплируем высоту террейна отдельно, только для
        // визуального маркера, чтобы существо реально стояло на поверхности.
        t.position = {c.position.x, m_terrain.heightAt(c.position.x, c.position.z), c.position.z};
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
