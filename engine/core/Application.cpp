#include "core/Application.hpp"

#include "core/InputSystem.hpp"
#include "core/Logger.hpp"
#include "core/TerrainChunks.hpp"
#include "platform/Paths.hpp"
#include "procgen/AnimalGen.hpp"
#include "procgen/HouseGen.hpp"
#include "ui/Style.hpp"
#include "renderer/Frustum.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/Window.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <random>
#include <utility>

namespace wsim {

namespace {

// Небо/горизонт: цвет тумана (линейный, до тонемапа). Фон кадра -- он же после
// тонемапа и гаммы (как tonemapEncode в lighting.glsl), чтобы вдали рельеф
// растворялся в небе без шва.
constexpr glm::vec3 kSkyColor{0.55f, 0.70f, 0.88f};
glm::vec4 encodedSky() {
    glm::vec3 c = kSkyColor / (kSkyColor + glm::vec3(1.0f));
    return {std::pow(c.r, 1.0f / 2.2f), std::pow(c.g, 1.0f / 2.2f), std::pow(c.b, 1.0f / 2.2f), 1.0f};
}

// Маркер существа: маленькая пирамида (квадратное основание + смещённая
// вперёд/вверх вершина), чтобы форма сама по себе читалась как "смотрит
// вперёд" в паре с Transform::rotation.y (см. Application::update()).
// Нормали общие на вершину, как и у buildGroundPlane() -- освещение пока
// только ambient (см. m_lights ниже), точность нормалей по граням не видна.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildCreatureMesh() {
    const glm::vec4 normal{0.f, 1.f, 0.f, 0.f};
    const glm::vec4 color{0.85f, 0.7f, 0.3f, 0.75f};  // alpha 0.75 -- материал существа (materials.glsl)
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
        1, 0, 4,           // бока (CCW снаружи)
        2, 1, 4,
        3, 2, 4,
        0, 3, 4,
    };
    return {std::move(verts), std::move(indices)};
}

// Валун: икосфера (2 деления, 320 граней) с радиальным шумом -- неровная "картошка" единичного
// радиуса. Грани плоские (у каждой своя нормаль), цвет серо-бурый с вариацией по граням. Окрашен
// как простой материал (alpha 1.0), без процедурной текстуры: она привязана к миру и "плыла"
// бы по вращающемуся телу.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> buildBlobMesh(glm::vec3 base, float roughness, bool berries,
                                                                    glm::vec3 berryColor = {0.78f, 0.13f, 0.15f}, float berryShare = 0.10f) {
    std::vector<glm::vec3> pts;
    std::vector<std::array<int, 3>> faces;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    for (glm::vec3 v : {glm::vec3{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}})
        pts.push_back(glm::normalize(v));
    faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
             {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 0; level < 2; ++level) {
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            if (const auto it = mid.find(key); it != mid.end()) return it->second;
            pts.push_back(glm::normalize(pts[static_cast<std::size_t>(a)] + pts[static_cast<std::size_t>(b)]));
            return mid[key] = static_cast<int>(pts.size()) - 1;
        };
        std::vector<std::array<int, 3>> next;
        for (const auto& f : faces) {
            const int ab = midpoint(f[0], f[1]), bc = midpoint(f[1], f[2]), ca = midpoint(f[2], f[0]);
            next.push_back({f[0], ab, ca});
            next.push_back({f[1], bc, ab});
            next.push_back({f[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        faces = std::move(next);
    }
    // Неровность: радиус каждой вершины по детерминированному хэшу направления.
    auto hash = [](const glm::vec3& p) {
        const float h = std::sin(p.x * 127.1f + p.y * 311.7f + p.z * 74.7f) * 43758.5453f;
        return h - std::floor(h);
    };
    for (auto& p : pts) p *= (1.0f - 0.5f * roughness) + roughness * hash(p);

    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (const auto& f : faces) {
        glm::vec3 a = pts[static_cast<std::size_t>(f[0])], b = pts[static_cast<std::size_t>(f[1])], c = pts[static_cast<std::size_t>(f[2])];
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        if (glm::dot(n, a + b + c) < 0.0f) { std::swap(b, c); n = -n; }  // CCW снаружи
        const float tone = 0.78f + 0.4f * hash(a + b + c);
        glm::vec4 col(base * tone, 1.0f);
        if (berries && hash(a * 3.1f + b * 1.7f + c) > 1.0f - berryShare) col = glm::vec4(berryColor, 1.0f);  // ягоды: редкие грани цвета вида
        const auto base = static_cast<uint32_t>(verts.size());
        for (const glm::vec3& p : {a, b, c}) verts.push_back({glm::vec4(p, 0.f), glm::vec4(n, 0.f), col});
        idx.insert(idx.end(), {base, base + 1, base + 2});
    }
    return {std::move(verts), std::move(idx)};
}

constexpr float kAnimalVisualScale = 1.6f;  // модели зверей крупнее "настоящих" размеров SpeciesDef (камера высоко)

// WSIM_SEED=N фиксирует сид мира при запуске (сравнение картинок/замеры); иначе случайный.
unsigned startSeed() {
    if (const char* e = std::getenv("WSIM_SEED")) return static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    return std::random_device{}();
}

}  // namespace

// ─── Constructor ──────────────────────────────────────────────────────────────
Application::Application(AppConfig config)
    : m_config(config),
      m_world(startSeed()),
      m_menuWorld(std::random_device{}()),
      m_terrain(World::kWorldHalfSize, startSeed()) {
    Logger::info("Initializing WorldSimulator core");

    m_window  = std::make_unique<Window>(config.title, config.width, config.height);
    m_context = std::make_unique<VulkanContext>(*m_window, config.vsync, std::string(config.shaderDir));
    m_context->setClearColor(encodedSky());
    m_input   = std::make_unique<InputSystem>(m_window->handle());
    m_imgui   = std::make_unique<ImGuiLayer>(m_window->handle(), *m_context, std::string(config.assetDir));
    // Генерация кадров рисует интерфейс дважды за кадр (на промежуточном и настоящем изображении)
    m_context->setUiRecorder([this](VkCommandBuffer cmd) { m_imgui->renderDrawData(cmd); });
    m_menu    = std::make_unique<MainMenu>(MenuFonts{m_imgui->titleFont(), m_imgui->uiFont()});
    m_menu->setCaps(static_cast<int>(m_context->maxMsaaSamples()), m_context->rayQuerySupported());

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_context->physDevice(), &props);
        // Стартовая точка автодальности по классу видеокарты, дальше подстраивается по FPS.
        m_autoRadius = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 24.0f
                     : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 10.0f
                                                                                  : 8.0f;
    }

    m_audio = std::make_unique<audio::AudioEngine>();
    m_sound = std::make_unique<SoundDirector>(*m_audio);
    m_menu->setSoundCallback([this](UiSound s) { m_sound->ui(s); });
    m_menu->setAudioStatus(m_audio->status());
    if (const char* dir = std::getenv("WSIM_DUMP_SOUNDS")) m_audio->dumpBank(dir);  // послушать звуки: WAV в каталог

    m_powerBar = std::make_unique<PowerBar>(MenuFonts{m_imgui->titleFont(), m_imgui->uiFont()});
    m_powerBar->setSoundCallback([this](UiSound s) { m_sound->ui(s); });
    for (auto& power : makeBuiltinPowers()) m_powers.add(std::move(power));
    m_brush = m_powers.all().front().radius;

    // Моды: папка рядом с игрой и пользовательская; список выключенных лежит рядом с настройками.
    {
        namespace fs = std::filesystem;
        std::vector<fs::path> roots;
        if (!config.modsDir.empty()) roots.emplace_back(config.modsDir);
        roots.push_back(paths::dataDir() / "WorldSimulator" / "mods");
        m_mods = std::make_unique<ModManager>(roots, fs::path(Settings::defaultPath()).parent_path() / "disabled_mods.cfg");
        m_mods->reload(m_powers);
        std::string hint = "Папки модов: ";
        for (std::size_t i = 0; i < roots.size(); ++i) hint += (i ? "  ·  " : "") + roots[i].string();
        m_menu->setModsHint(std::move(hint));
    }

    m_saves = std::make_unique<SaveStore>(paths::dataDir() / "WorldSimulator" / "saves");
    m_menu->setSavesHint("Папка: " + m_saves->dir().string());
    refreshSaves();
    startedNewWorld();

    m_settingsPath = Settings::defaultPath();
    m_settings     = Settings::load(m_settingsPath);
    // Отладка без меню: WSIM_UPSCALER=0..3 (3 = temporal), WSIM_RENDER_SCALE=0.5..1, WSIM_FRAMEGEN=1 (нужен vsync)
    if (const char* e = std::getenv("WSIM_UPSCALER")) m_settings.upscaler = std::atoi(e);
    if (const char* e = std::getenv("WSIM_RENDER_SCALE")) m_settings.renderScale = static_cast<float>(std::atof(e));
    if (std::getenv("WSIM_FRAMEGEN")) m_settings.frameGen = true;
    m_saved        = m_settings;

    // Рельеф + вода строятся чанками вокруг камеры (см. TerrainChunks, render()).
    m_chunks = std::make_unique<TerrainChunks>(*m_context, m_terrain);
    // Деревья не растут на домах и на кустах с едой (проверяется при построении чанка; дом ставится --
    // силой "Дом" выравнивается площадка, чанк пересобирается и деревья там исчезают).
    m_chunks->setTreeBlocker([this](float x, float z, float radius) {
        for (const Building& b : m_world.buildings())
            if (std::hypot(b.x - x, b.z - z) < radius + 4.2f) return true;
        // Кусты и спящих ячеек (процедурные) тоже: дерево не должно вырасти на месте будущего куста.
        return m_world.bushNear(m_terrain, x, z, radius + 0.6f);
    });
    m_world.settleOnLand(m_terrain);  // стартовые существа -- на суше, не в воде
    m_world.growBerryBushes(m_terrain);  // кусты по биомам (пятна, виды ягод)
    m_world.growWildlife(m_terrain);     // дикие животные по биомам

    m_applied.msaa = -1;  // заведомо не равно загруженным -- первый вызов применит всё
    applySettings();

    // Один общий меш на всех существ — SceneObject'ы (см. update()) просто
    // ссылаются на него с разным Transform, ни одной лишней GPU-аллокации.
    auto [creatureV, creatureI] = buildCreatureMesh();
    m_creatureMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        creatureV, creatureI));

    auto [boulderV, boulderI] = buildBlobMesh({0.43f, 0.40f, 0.37f}, 0.26f, false);
    m_boulderMesh = std::make_shared<Mesh>(Mesh::create(
        m_context->physDevice(), m_context->device(),
        m_context->commandPool(), m_context->graphicsQueue(),
        boulderV, boulderI));

    {
        // Кусты с едой: лиственные "картошки" с ягодами своего цвета (по виду, см. sim/Berries); размер показывает,
        // сколько ягод ещё осталось. Черника -- с густыми мелкими ягодами, морошка -- с редкими крупными.
        constexpr float kShare[kBerryKindCount] = {0.10f, 0.17f, 0.12f, 0.08f, 0.12f, 0.10f, 0.13f, 0.14f, 0.15f, 0.11f, 0.09f};
        for (int k = 0; k < kBerryKindCount; ++k) {
            const BerryDef& def = berryDef(static_cast<BerryKind>(k));
            auto [bushV, bushI] = buildBlobMesh(def.leafColor, 0.4f, true, def.berryColor, kShare[k]);
            m_bushMeshes[static_cast<std::size_t>(k)] = std::make_shared<Mesh>(Mesh::create(
                m_context->physDevice(), m_context->device(),
                m_context->commandPool(), m_context->graphicsQueue(),
                bushV, bushI));
        }
    }
    for (int sp = 0; sp < kSpeciesCount; ++sp) {  // дикие животные: модель на вид
        const auto model = procgen::buildAnimal(static_cast<Species>(sp));
        m_animalMeshes[static_cast<std::size_t>(sp)] = std::make_shared<Mesh>(Mesh::create(
            m_context->physDevice(), m_context->device(), m_context->commandPool(), m_context->graphicsQueue(), model.vertices, model.indices));
    }
    for (std::uint32_t i = 0; i < kHouseVariants; ++i) {
        const auto house = procgen::buildHouse(procgen::houseVariant(i));
        m_houseMeshes.push_back(std::make_shared<Mesh>(Mesh::create(
            m_context->physDevice(), m_context->device(),
            m_context->commandPool(), m_context->graphicsQueue(),
            house.vertices, house.indices)));
    }

    // Освещение: ambient + солнце (направленный свет — иначе рельеф не
    // читается как рельеф, только силуэтом, см. CLAUDE.md). Фиксированный
    // угол, без дня/ночи (вне охвата этого раунда).
    m_lights.ambient          = {1.0f, 1.0f, 1.0f, 0.6f};
    m_lights.sunDir            = glm::normalize(glm::vec4(0.6f, 0.7f, 0.4f, 0.0f));  // ~44 градуса над горизонтом: тени читаются
    m_lights.sunColorIntensity = {1.0f, 0.98f, 0.92f, 0.9f};

    // WSIM_NIGHT=1 -- без солнца и с тусклым ambient (проверка фонаря до появления смены дня и ночи).
    if (std::getenv("WSIM_NIGHT")) { m_lights.sunColorIntensity.w = 0.0f; m_lights.ambient.w = 0.12f; }
    if (std::getenv("WSIM_FLASHLIGHT")) m_flashlight = true;

    // WSIM_DEMO_HILLS=1 -- несколько гор рядом с центром (проверка теней/снега без мышки).
    if (std::getenv("WSIM_DEMO_HILLS")) {
        for (int i = 0; i < 22; ++i) {
            m_terrain.raise(10.0f, 10.0f, 18.0f, 1.0f);
            m_terrain.raise(-25.0f, 5.0f, 12.0f, 1.2f);
            m_terrain.raise(30.0f, -20.0f, 8.0f, 0.9f);
        }
    }

    // WSIM_DEMO_BOULDERS=1 -- несколько валунов над холмами (проверка физики и теней без мышки).
    if (std::getenv("WSIM_DEMO_BOULDERS")) {
        for (int i = 0; i < 6; ++i) {
            const float x = 8.0f + 3.0f * static_cast<float>(i % 3), z = 6.0f + 3.0f * static_cast<float>(i / 3);
            m_physics.addSphere({x, m_terrain.heightAt(x, z) + 18.0f + 3.0f * static_cast<float>(i), z}, 1.0f + 0.25f * static_cast<float>(i % 3));
        }
    }

    // WSIM_DEMO_HOUSES=1 -- несколько домов вокруг центра (проверка procgen-построек без мышки).
    if (std::getenv("WSIM_DEMO_HOUSES")) {
        GodContext god(m_world, m_terrain);
        int placed = 0;
        for (float r = 8.0f; r < 60.0f && placed < 7; r += 3.0f)
            for (float a = 0.0f; a < 6.28f && placed < 7; a += 0.9f)
                if (god.build(std::cos(a) * r, std::sin(a) * r, a * 57.3f)) ++placed;
        for (const auto& rect : god.takeDirty()) (void)rect;  // чанки ещё не созданы -- построятся уже с правками
    }

    // WSIM_MENU_PAGE=load | settings:N -- открыть страницу меню сразу (для снимков интерфейса через WSIM_FRAME).
    if (const char* page = std::getenv("WSIM_MENU_PAGE")) {
        const std::string p = page;
        if (p == "load") m_menu->debugOpen(1, 0);
        else if (p.rfind("settings", 0) == 0) m_menu->debugOpen(0, p.size() > 9 ? std::atoi(p.c_str() + 9) : 0);
    }

    // WSIM_CAM_AT=x,z[,расстояние[,pitch]] -- камера на точку мира (обзор биомов: WSIM_CAM_AT=0,0,200,-55).
    if (const char* at = std::getenv("WSIM_CAM_AT")) {
        float x = 0, z = 0, dist = 60, pitch = -45;
        std::sscanf(at, "%f,%f,%f,%f", &x, &z, &dist, &pitch);
        m_camera.restore({x, m_terrain.heightAt(x, z), z}, -90.0f, pitch, dist);
    }

    // WSIM_ZOO=1 -- убрать зверей и выпустить по одному каждого вида рядком у точки обзора (проверка моделей).
    if (std::getenv("WSIM_ZOO")) {
        m_world.removeAnimals(0.0f, 0.0f, 1.0e4f);
        const glm::vec3 c = m_camera.target();
        for (int i = 0; i < kSpeciesCount; ++i) {
            const float x = c.x - 14.0f + 4.0f * static_cast<float>(i);
            for (float dz = 0.0f; dz < 40.0f; dz += 3.0f)  // ближайшая суша вдоль линии
                if (m_world.spawnAnimal(static_cast<Species>(i), x, c.z + dz) || m_world.spawnAnimal(static_cast<Species>(i), x, c.z - dz)) break;
        }
    }

    // WSIM_CAM_BUSH=raspberry|blueberry|blackberry|cloudberry|tropical|cactus -- камера на ближайший к центру куст этого вида.
    if (const char* id = std::getenv("WSIM_CAM_BUSH")) {
        BerryKind want{};
        if (berryFromId(id, want)) {
            float best = 1e9f;
            for (const FoodSource& f : m_world.food()) {
                const float d = std::hypot(f.position.x, f.position.z);
                if (f.kind == want && d < best) {
                    best = d;
                    m_camera.restore({f.position.x, m_terrain.heightAt(f.position.x, f.position.z), f.position.z}, -90.0f, -35.0f,
                                     std::getenv("WSIM_CAM_DIST") ? std::strtof(std::getenv("WSIM_CAM_DIST"), nullptr) : 10.0f);
                }
            }
        }
    }

    // WSIM_CAM_TREE=oak|birch|spruce|pine|acacia|cactus|palm -- камера на ближайшее к центру дерево этого вида (проверка моделей деревьев).
    if (const char* kind = std::getenv("WSIM_CAM_TREE")) {
        const std::string name = kind;
        TreeSpecies want = TreeSpecies::Oak;
        if (name == "birch") want = TreeSpecies::Birch;
        else if (name == "pine") want = TreeSpecies::Pine;
        else if (name == "spruce") want = TreeSpecies::Spruce;
        else if (name == "acacia") want = TreeSpecies::Acacia;
        else if (name == "cactus") want = TreeSpecies::Cactus;
        else if (name == "palm") want = TreeSpecies::Palm;
        float best = 1e9f;
        glm::vec3 at{0.0f};
        for (const TreeInstance& t : Trees::inRect(m_terrain, -600, -600, 600, 600)) {
            const float d = std::hypot(t.x, t.z);
            if (t.species == want && d < best) { best = d; at = {t.x, m_terrain.heightAt(t.x, t.z) + 2.0f, t.z}; }
        }
        m_camera.restore(at, -90.0f, -30.0f, std::getenv("WSIM_CAM_DIST") ? std::strtof(std::getenv("WSIM_CAM_DIST"), nullptr) : 14.0f);
    }

    // WSIM_LOAD_SLOT=слот -- сразу загрузить сохранение (и войти в игру).
    if (const char* slot = std::getenv("WSIM_LOAD_SLOT")) {
        m_started = true;  // loadFromSlot сам включит экран игры
        const bool ok = loadFromSlot(slot);
        Logger::info("Загрузка слота {}: {} ({} существ, {} домов)", slot, ok ? "ok" : "ОШИБКА", m_world.populationCount(), m_world.buildings().size());
    }

    // WSIM_AUTOSTART=1 -- сразу в игру, мимо меню (смоук-тест игрового цикла вместе с WSIM_AUTOEXIT).
    if (std::getenv("WSIM_AUTOSTART")) { m_started = true; m_screen = Screen::Playing; }

    Logger::info("World: {} creatures, {} food sources", m_world.populationCount(), m_world.foodCount());
    Logger::info("Engine ready");
}

Application::~Application() {
    Logger::info("Shutting down");
    if (!(m_settings == m_saved)) m_settings.save(m_settingsPath);
    if (m_context) {
        vkDeviceWaitIdle(m_context->device());
        m_chunks.reset();  // освобождает меши чанков (device уже idle)
        // m_creatureObjects делят один и тот же m_creatureMesh -- удаляем
        // его РОВНО один раз, а не через перебор объектов сцены (иначе
        // Mesh::destroy() вызвался бы на одних и тех же GPU-буферах
        // многократно -- double free).
        if (m_creatureMesh) m_creatureMesh->destroy(m_context->device());
        if (m_boulderMesh) m_boulderMesh->destroy(m_context->device());
        for (auto& mesh : m_bushMeshes)
            if (mesh) mesh->destroy(m_context->device());
        for (auto& mesh : m_animalMeshes)
            if (mesh) mesh->destroy(m_context->device());
        for (auto& mesh : m_houseMeshes) mesh->destroy(m_context->device());
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
    // WSIM_AUTOEXIT_SECONDS=N -- другая длительность смоук-теста (по умолчанию 5 с).
    const float autoExitAfter = std::getenv("WSIM_AUTOEXIT_SECONDS") ? std::strtof(std::getenv("WSIM_AUTOEXIT_SECONDS"), nullptr) : 5.0f;
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
        // WSIM_SAVE_SMOKE=слот -- на 2.5 с сохранить мир в слот (проверка сохранения без интерфейса).
        static bool saved = false;
        if (const char* slot = std::getenv("WSIM_SAVE_SMOKE"); slot && !saved && totalTime > 2.5f) {
            saved = true;
            const bool ok = saveToSlot(slot, m_worldName + " (тест)");
            if (ok) showToast("Быстрое сохранение");
            Logger::info("Сохранение в слот {}: {} ({} существ, {} домов, {} валунов)", slot, ok ? "ok" : "ОШИБКА", m_world.populationCount(),
                         m_world.buildings().size(), m_physics.bodies().size());
        }
        // WSIM_INSPECT=1 -- на 1.5 с выбрать для осмотра ближайшее к центру существо (проверка панели).
        static bool inspected = false;
        if (std::getenv("WSIM_INSPECT") && !inspected && totalTime > 1.5f) {
            inspected = true;
            float best = 1e9f;
            for (const Inhabitant& in : m_world.population()) {
                const float d = std::hypot(in.creature.position.x, in.creature.position.z);
                if (d < best) { best = d; m_inspectId = in.creature.id; }
            }
        }
        // WSIM_INSPECT_BUSH=1 -- то же для ближайшего к центру ягодного куста (панель куста).
        static bool inspectedBush = false;
        if (std::getenv("WSIM_INSPECT_BUSH") && !inspectedBush && totalTime > 1.5f && m_world.foodCount() > 0) {
            inspectedBush = true;
            float best = 1e9f;
            for (const FoodSource& f : m_world.food()) {
                const float d = std::hypot(f.position.x, f.position.z);
                if (d < best) { best = d; m_inspectBush = glm::vec2(f.position.x, f.position.z); }
            }
        }
        // WSIM_SCREENSHOT=файл.ppm -- снимок сцены на ~3-й секунде (самопроверка визуала без захвата экрана).
        static bool shot = false;
        if (const char* path = std::getenv("WSIM_SCREENSHOT"); path && !shot && totalTime > 3.0f) {
            m_context->requestSceneDump(path);
            shot = true;
        }
        // WSIM_FRAME=файл.ppm -- готовый кадр со всем интерфейсом (меню, панель сил); WSIM_FRAME_AT=секунды (по умолчанию 3).
        static bool frameShot = false;
        const float frameAt = std::getenv("WSIM_FRAME_AT") ? std::strtof(std::getenv("WSIM_FRAME_AT"), nullptr) : 3.0f;
        if (const char* path = std::getenv("WSIM_FRAME"); path && !frameShot && totalTime > frameAt) {
            m_context->requestFrameDump(path);
            frameShot = true;
        }
        if (autoExit && totalTime > autoExitAfter) glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);
    }
}

// ─── processInput ─────────────────────────────────────────────────────────────
void Application::processInput(float dt) {
    m_input->update();

    if (m_input->isKeyPressed(GLFW_KEY_F10)) glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);

    // Esc: из игры в меню и обратно (из меню -- только если игра уже начата).
    if (m_input->isKeyPressed(GLFW_KEY_ESCAPE)) {
        if (m_screen == Screen::MainMenu && m_menu->inSettings()) { m_menu->back(); m_sound->ui(UiSound::Back); }
        else if (m_screen == Screen::Playing)                     m_screen = Screen::MainMenu;
        else if (m_started)                                       m_screen = Screen::Playing;
    }
    if (m_screen == Screen::MainMenu) { m_toolDown = false; m_toolFirst = true; return; }  // камера не слушает ввод, пока открыто меню

    // Панорамирование: WASD, масштабируется дистанцией (быстрее при отдалении).
    const float panAmount = m_settings.panSpeed * m_camera.distance() / 25.0f * dt;
    float forward = 0.f, right = 0.f;
    if (m_input->isKeyDown(GLFW_KEY_W)) forward += panAmount;
    if (m_input->isKeyDown(GLFW_KEY_S)) forward -= panAmount;
    if (m_input->isKeyDown(GLFW_KEY_D)) right   += panAmount;
    if (m_input->isKeyDown(GLFW_KEY_A)) right   -= panAmount;
    if (forward != 0.f || right != 0.f) m_camera.pan(forward, right);

    // Вращение: зажатая ПКМ + движение мыши.
    if (m_input->isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        const float dy = m_settings.invertOrbit ? m_input->mouseDY() : -m_input->mouseDY();
        m_camera.orbit(m_input->mouseDX() * m_settings.orbitSens, dy * m_settings.orbitSens);
    }

    if (m_input->isKeyPressed(GLFW_KEY_F)) { m_flashlight = !m_flashlight; m_sound->ui(UiSound::Toggle); }
    if (m_input->isKeyPressed(GLFW_KEY_F5)) {
        if (saveToSlot("quick", m_worldName + " (быстрое)")) showToast("Быстрое сохранение");
    }
    if (m_input->isKeyPressed(GLFW_KEY_F9)) {
        if (m_saves->exists("quick")) loadFromSlot("quick");
        else showToast("Нет быстрого сохранения (F5)", true);
    }

    // Силы бога: 1..9 -- выбор, [ ] -- радиус кисти, Ctrl+колесо -- тоже радиус.
    const auto powers = m_powers.all();
    for (int k = 0; k < 10 && k < static_cast<int>(powers.size()); ++k)
        if (m_input->isKeyPressed(k < 9 ? GLFW_KEY_1 + k : GLFW_KEY_0)) {   // 1..9, десятая сила -- на 0
            if (m_power != k) m_sound->ui(UiSound::Click);
            m_power = k;
            m_brush = std::clamp(m_brush, powers[static_cast<std::size_t>(k)].minRadius, powers[static_cast<std::size_t>(k)].maxRadius);
        }
    const PowerDef& power = powers[static_cast<std::size_t>(std::min(m_power, static_cast<int>(powers.size()) - 1))];
    if (m_input->isKeyDown(GLFW_KEY_LEFT_BRACKET))  m_brush *= std::exp(-dt * 1.4f);
    if (m_input->isKeyDown(GLFW_KEY_RIGHT_BRACKET)) m_brush *= std::exp(dt * 1.4f);

    // Зум колесом (с Ctrl -- радиус кисти).
    const float scroll = m_input->scrollDY();
    if (scroll != 0.f) {
        if (m_input->isKeyDown(GLFW_KEY_LEFT_CONTROL) || m_input->isKeyDown(GLFW_KEY_RIGHT_CONTROL)) m_brush *= std::exp(scroll * 0.1f);
        else                                                                                         m_camera.zoom(-scroll * m_settings.zoomStep);
    }
    m_brush = std::clamp(m_brush, power.minRadius, power.maxRadius);

    // ЛКМ вне интерфейса -- рабочая рука. Состояние читает update() (фиксированный шаг).
    const bool lmb = m_input->isMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse && !m_barHover;
    if (!lmb) { m_toolFirst = true; m_toolTimer = 0.0f; }
    m_toolDown = lmb;
}

// ─── update ───────────────────────────────────────────────────────────────────
void Application::update(float dt) {
    if (m_screen == Screen::MainMenu) {
        m_menuWorld.tick(dt);  // декоративная колония, настоящий мир стоит
        return;
    }
    m_playTime += dt;
    // Отладка: WSIM_CAM_SPIN=градусов/с -- камера сама вращается (проверка temporal-апскейлера и генерации кадров в движении)
    static const float spin = std::getenv("WSIM_CAM_SPIN") ? std::strtof(std::getenv("WSIM_CAM_SPIN"), nullptr) : 0.0f;
    if (spin != 0.0f) m_camera.orbit(spin * dt, 0.0f);
    if (m_settings.autosaveMinutes > 0) {
        m_autosaveTimer += dt;
        if (m_autosaveTimer >= static_cast<float>(m_settings.autosaveMinutes) * 60.0f) {
            m_autosaveTimer = 0.0f;
            if (saveToSlot("autosave", m_worldName + " (авто)")) showToast("Автосохранение");
        }
    }
    applyPower(dt);
    stepPhysics(dt);
    // Мир "живёт" не только вокруг существ: камера тоже якорь -- рядом с ней просыпаются кусты и заселяются звери.
    {
        const glm::vec3 cam = m_camera.target();
        const glm::vec2 focus{cam.x, cam.z};
        m_world.setFocus(std::span<const glm::vec2>(&focus, 1));
    }
    m_world.tick(dt, &m_terrain);
    for (const auto& rect : m_world.takeDirty()) { if (m_chunks) m_chunks->invalidate(rect); }   // срубленные деревья: чанки пересобираются

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
        m_creatureObjects.back().trackId = c.id;
    }

    // Постройки: вид по зерну, посажены на землю (если рельеф под ними меняют, дом едет вместе с землёй).
    m_buildingObjects.clear();
    for (const Building& b : m_world.buildings()) {
        Transform t;
        t.position   = {b.x, m_terrain.heightAt(b.x, b.z), b.z};
        t.rotation.y = b.yaw;
        m_buildingObjects.push_back({m_houseMeshes[b.seed % kHouseVariants], t, /*solid=*/false});
    }

    // Дикие животные: модель вида на земле, развёрнута по курсу (морда +Z), детёныши меньше, на бегу чуть подпрыгивают.
    m_animalObjects.clear();
    for (const Animal& a : m_world.fauna().animals()) {
        const SpeciesDef& d = speciesDef(a.species);
        Transform t;
        const float growth = 0.55f + 0.45f * std::min(1.0f, a.age / d.maturity);
        const float bob = a.speedNow > 0.4f ? 0.05f * d.size * std::fabs(std::sin(a.age * (4.0f + 1.2f * a.speedNow))) : 0.0f;
        t.position   = {a.position.x, m_terrain.heightAt(a.position.x, a.position.z) + bob, a.position.z};
        t.rotation.y = glm::degrees(std::atan2(std::cos(a.heading), std::sin(a.heading)));
        t.scale      = glm::vec3(d.size * growth * kAnimalVisualScale);   // с высоты god-камеры настоящие размеры слишком мелки
        m_animalObjects.push_back({m_animalMeshes[static_cast<std::size_t>(a.species)], t, /*solid=*/false});
        m_animalObjects.back().trackId = a.id;
    }

    // Еда: кусты на земле, ужимаются, когда их объедают (пустой -- маленький росток).
    m_foodObjects.clear();
    for (const FoodSource& f : m_world.food()) {
        Transform t;
        const float scale = 0.35f + 0.75f * std::clamp(f.amount, 0.0f, 1.0f);
        t.position = {f.position.x, m_terrain.heightAt(f.position.x, f.position.z) + 0.55f * scale, f.position.z};
        t.scale    = glm::vec3(scale * 1.15f, scale * 0.9f, scale * 1.15f);
        m_foodObjects.push_back({m_bushMeshes[static_cast<std::size_t>(f.kind)], t, /*solid=*/false});
    }
}

// ─── render ───────────────────────────────────────────────────────────────────
// WSIM_PERF=1: раз в 2 с в лог -- FPS, CPU-время render() и число draw-вызовов.
namespace {
struct PerfStats {
    int shadowDraws = 0, sceneDraws = 0;
    int cascadeDraws[kShadowCascades]{};
    int cascade = 0;
};
PerfStats g_perf;
}  // namespace

void Application::render() {
    static const bool perfOn = std::getenv("WSIM_PERF") != nullptr;
    static auto       perfT0 = std::chrono::steady_clock::now();
    static int        perfFrames = 0;
    static double     perfCpu = 0.0;
    const auto        cpuStart = std::chrono::steady_clock::now();
    g_perf = {};
    m_imgui->newFrame();
    if (m_screen == Screen::MainMenu) {
        // Колония в чашке меню -- первые kInitialPopulation существ декоративного
        // мира (дети идут в конец вектора, так что видны "исходные"), позиции
        // нормированы в круг.
        std::vector<glm::vec2> colony;
        colony.reserve(World::kInitialPopulation);
        for (const auto& inhabitant : m_menuWorld.population()) {
            if (colony.size() >= World::kInitialPopulation) break;
            glm::vec2 p{inhabitant.creature.position.x, inhabitant.creature.position.z};
            p /= World::kWorldHalfSize;
            if (const float len = glm::length(p); len > 1.0f) p /= len;
            colony.push_back(p);
        }
        m_barHover = false;
        std::vector<ModRow> modRows;
        syncModRows(modRows);
        // Список сохранений: кэш обновляется раз в ~1.5 с, пока открыто меню.
        m_saveRefresh -= ImGui::GetIO().DeltaTime;
        if (m_saveRefresh <= 0.0f) refreshSaves();
        std::vector<SaveRow> saveRows;
        for (const SaveEntry& e : m_saveEntries) {
            SaveRow row;
            row.name  = e.meta.name.empty() ? e.slot : e.meta.name;
            row.valid = e.valid;
            row.error = e.valid ? std::string() : e.error;
            if (e.valid) {
                char when[32] = "";
                const std::time_t t = static_cast<std::time_t>(e.meta.unixTime);
                if (const std::tm* tm = std::localtime(&t)) std::strftime(when, sizeof when, "%d.%m.%Y %H:%M", tm);
                row.subtitle = std::string(when) + " · существ " + std::to_string(e.meta.population) + " · домов " +
                               std::to_string(e.meta.buildings) + " · игра " + std::to_string(static_cast<int>(e.meta.playSeconds / 60.0f)) + " мин";
            }
            saveRows.push_back(std::move(row));
        }
        const MenuAction menuAction = m_menu->draw(ImGui::GetIO().DeltaTime, m_started, colony, m_settings, modRows, saveRows);
        for (std::size_t i = 0; i < modRows.size(); ++i)
            if (modRows[i].toggled) {
                m_mods->setEnabled(m_powers, m_mods->mods()[i].id, modRows[i].enabled);
                m_power = 0;  // выбранная сила могла исчезнуть вместе с модом
            }
        switch (menuAction) {
            case MenuAction::NewWorld:
                // Уже начатую игру "Новый мир" начинает заново (пока без подтверждения --
                // сохранений нет): новые существа и новый рельеф.
                if (m_started) {
                    m_world   = World(std::random_device{}());
                    m_terrain = Terrain(World::kWorldHalfSize, std::random_device{}());
                    m_chunks->reset();
                }
                m_world.settleOnLand(m_terrain);
                m_world.growBerryBushes(m_terrain);
                m_world.growWildlife(m_terrain);
                m_physics.clear();
                m_boulderObjects.clear();
                m_buildingObjects.clear();
                m_started = true;
                m_screen  = Screen::Playing;
                startedNewWorld();
                m_context->resetTemporalHistory();  // другой мир -- истории нечего делать
                break;
            case MenuAction::Continue: m_screen = Screen::Playing; break;
            case MenuAction::Save:     saveCurrent(); break;
            case MenuAction::LoadSave: {
                const int idx = m_menu->actionIndex();
                if (idx >= 0 && idx < static_cast<int>(m_saveEntries.size())) loadFromSlot(m_saveEntries[static_cast<std::size_t>(idx)].slot);
                break;
            }
            case MenuAction::DeleteSave: {
                const int idx = m_menu->actionIndex();
                if (idx >= 0 && idx < static_cast<int>(m_saveEntries.size())) {
                    m_saves->remove(m_saveEntries[static_cast<std::size_t>(idx)].slot);
                    showToast("Сохранение удалено");
                    refreshSaves();
                }
                break;
            }
            case MenuAction::Quit:
                // Выход из игры тоже сохраняет (в отдельный слот), чтобы закрытие окна не стоило мира.
                if (m_started && m_settings.autosaveMinutes > 0) saveToSlot("autosave", m_worldName + " (авто)");
                glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);
                break;
            case MenuAction::None:     break;
        }
    } else {
        if (m_settings.showDebug) renderDebugUI();
        std::vector<PowerBarItem> items;
        for (const auto& p : m_powers.all()) items.push_back({p.name, p.description, p.source});
        const PowerDef& cur = m_powers.all()[static_cast<std::size_t>(std::min(m_power, static_cast<int>(m_powers.all().size()) - 1))];
        m_barHover = m_powerBar->draw(ImGui::GetIO().DeltaTime, items, m_power, m_brush, cur.minRadius, cur.maxRadius);
        m_brush = std::clamp(m_brush, cur.minRadius, cur.maxRadius);
    }

    drawToast(ImGui::GetIO().DeltaTime);
    applySettings();
    if (m_screen == Screen::Playing) updateAutoViewRadius(ImGui::GetIO().DeltaTime);
    m_chunks->setViewRadius(m_settings.autoViewRadius
                                ? std::clamp(static_cast<int>(m_autoRadius), 4, m_settings.viewRadius)
                                : m_settings.viewRadius);
    // Сохраняем, когда мышь отпущена: во время перетаскивания ползунка писать на диск не нужно.
    if (!(m_settings == m_saved) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_settings.save(m_settingsPath);
        m_saved = m_settings;
    }

    m_context->beginFrame();
    if (m_context->isFrameValid()) {
        const VkExtent2D ext = m_context->swapchainExtent();
        m_camera.setAspect(static_cast<float>(ext.width) / static_cast<float>(ext.height));
        m_sound->update(ImGui::GetIO().DeltaTime, m_screen == Screen::Playing, m_camera.target(), m_camera.front(), m_camera.distance(),
                        m_world, m_terrain);
        if (m_screen == Screen::Playing) followTerrain(ImGui::GetIO().DeltaTime);

        const CameraData camData = m_camera.getData();
        m_context->updateCamera(camData);

        const bool playing   = m_screen == Screen::Playing;
        const bool shadowsOn = playing && m_settings.shadows;
        if (playing) {
            m_chunks->update(m_camera.target());
            const ImVec2 mp = ImGui::GetIO().MousePos;
            m_cursorWorld   = (m_barHover || ImGui::GetIO().WantCaptureMouse) ? std::nullopt : pickTerrain(mp.x, mp.y);
            if (m_cursorWorld) drawBrushRing(camData);
            drawInspector(camData);
        }

        // Освещение кадра: время для воды, туман (он же горизонт), каскадные тени от солнца.
        m_lights.fogColorDensity = {kSkyColor, 0.0009f};
        m_lights.fogParams.x     = 0.015f;
        m_lights.fogParams.y     = static_cast<float>(ImGui::GetTime());
        CascadeSet cascades;
        if (shadowsOn) {
            cascades = computeCascades(camData.view, camData.proj, m_camera.nearPlane(),
                                       glm::normalize(glm::vec3(m_lights.sunDir)), VulkanContext::kShadowMapSize);
            for (std::size_t i = 0; i < kShadowCascades; ++i) {
                m_lights.cascadeVP[i]  = cascades.viewProj[i];
                m_lights.cascadeSplit[static_cast<int>(i)] = cascades.splitFar[i];
                m_lights.cascadeTexel[static_cast<int>(i)] = cascades.texelWorld[i];
            }
        }
        // Фонарь: прожектор от камеры на точку под курсором (или на центр обзора), конус ложится
        // на круг кисти. Затухание в шейдере ~1/(1+0.05 d^2), поэтому яркость растёт с дистанцией.
        if (m_flashlight && playing) {
            const glm::vec3 camPos = glm::vec3(camData.position);
            const glm::vec3 aim    = m_cursorWorld ? *m_cursorWorld : m_camera.target();
            const glm::vec3 dir    = glm::normalize(aim - camPos);
            const float     dist   = glm::length(aim - camPos);
            const float     litR   = std::max(m_brush * 1.6f, 10.0f);
            const float     outer  = std::atan2(litR, dist), inner = outer * 0.7f;
            m_lights.spotPosCosIn  = {camPos, std::cos(inner)};
            m_lights.spotDirCosOut = {dir, std::cos(outer)};
            m_lights.spotColor     = {1.0f, 0.93f, 0.78f, 3.5f * (1.0f + 0.05f * dist * dist)};
        } else {
            m_lights.spotColor = {};
        }
        m_lights.shadowParams = {shadowsOn ? 1.0f : 0.0f, 1.0f / static_cast<float>(VulkanContext::kShadowMapSize), 1.0f, 0.0f};
        m_context->updateLights(m_lights);

        const Frustum frustum = Frustum::fromMatrix(camData.proj * camData.view);

        // Тени: по проходу на каскад, в них рельеф (без воды) и существа.
        if (shadowsOn) {
            for (int i = 0; i < kShadowCascades; ++i) {
                g_perf.cascade = i;
                m_context->beginShadowCascade(i, cascades.viewProj[static_cast<std::size_t>(i)]);
                const Frustum lightFrustum = Frustum::fromMatrix(cascades.viewProj[static_cast<std::size_t>(i)]);
                // Далёкие каскады рисуют только то, что там видно: низкая равнина (на ней тень падает
                // и без неё -- карта глубины нужна лишь тем, что выше), деревья, существа и кусты
                // (в каскаде 3 тексель ~0.7 юнита) отбрасываются. Без этого каскады 2 и 3 давали
                // ~90% draw-вызовов теней (сотни чанков), а на картинке разницы нет.
                // WSIM_SHADOW_FULL=1 -- без отсечения (эталон для сравнения картинок).
                static const bool full = std::getenv("WSIM_SHADOW_FULL") != nullptr;
                constexpr float   kTerrainMinTop[kShadowCascades] = {-1e9f, -1e9f, 2.5f, 7.0f};
                const bool        far3 = !full && i == 3;
                drawShadowCasters(m_chunks->shadowCasters(), lightFrustum, full ? -1e9f : kTerrainMinTop[i]);
                if (!far3) drawShadowCasters(m_chunks->treeCasters(), lightFrustum);
                if (!far3) drawShadowCasters(m_creatureObjects, lightFrustum);
                if (!far3) drawShadowCasters(m_animalObjects, lightFrustum);
                drawShadowCasters(m_boulderObjects, lightFrustum);
                drawShadowCasters(m_buildingObjects, lightFrustum);
                if (!far3) drawShadowCasters(m_foodObjects, lightFrustum);
                m_context->endShadowCascade(i);
            }
        }
        m_context->beginScenePass();

        if (playing) {
            trackMotion(m_creatureObjects, m_prevCreaturePos);
            trackMotion(m_boulderObjects, m_prevBoulderPos);
            trackMotion(m_animalObjects, m_prevAnimalPos);
            drawObjects(m_chunks->objects(), frustum);
            drawObjects(m_creatureObjects, frustum);
            drawObjects(m_animalObjects, frustum);
            drawObjects(m_boulderObjects, frustum);
            drawObjects(m_buildingObjects, frustum);
            drawObjects(m_foodObjects, frustum);
        }

        m_context->beginUIPass();
        m_imgui->renderDrawData(m_context->currentCommandBuffer());
    }
    if (perfOn) perfCpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
    m_context->endFrame();
    if (perfOn) {
        ++perfFrames;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - perfT0).count();
        if (el >= 2.0) {
            Logger::info("PERF: {:.1f} fps ({:.2f} ms/frame), CPU render {:.2f} ms, draws shadow {} [{} {} {} {}] scene {}, chunks {}", perfFrames / el,
                         1000.0 * el / perfFrames, perfCpu / perfFrames, g_perf.shadowDraws, g_perf.cascadeDraws[0], g_perf.cascadeDraws[1], g_perf.cascadeDraws[2], g_perf.cascadeDraws[3], g_perf.sceneDraws, m_chunks->chunkCount());
            perfT0 = std::chrono::steady_clock::now();
            perfFrames = 0;
            perfCpu = 0.0;
        }
    }
}

// ─── trackMotion ──────────────────────────────────────────────────────────────
// Сколько каждый подвижный объект прошёл с прошлого НАРИСОВАННОГО кадра (а не шага симуляции):
// это и есть вектор движения для temporal-апскейлера. Новые объекты и "скачки" -- без движения.
void Application::trackMotion(std::vector<SceneObject>& objects, std::unordered_map<std::uint64_t, glm::vec3>& prev) {
    std::unordered_map<std::uint64_t, glm::vec3> now;
    now.reserve(objects.size());
    for (auto& obj : objects) {
        const auto it = prev.find(obj.trackId);
        obj.motion    = it != prev.end() ? obj.transform.position - it->second : glm::vec3(0.0f);
        if (glm::dot(obj.motion, obj.motion) > 30.0f * 30.0f) obj.motion = glm::vec3(0.0f);
        now[obj.trackId] = obj.transform.position;
    }
    prev = std::move(now);
}

// ─── drawObjects ──────────────────────────────────────────────────────────────
// Общий по фрустум-отсечению + drawMesh цикл — раньше жил только для m_scene,
// теперь его же используем для m_creatureObjects (см. render()).
void Application::drawObjects(const std::vector<SceneObject>& objects, const Frustum& frustum) {
    for (const auto& obj : objects) {
        if (!obj.mesh) continue;

        const glm::mat4 model = obj.transform.matrix();
        if (!frustum.intersects(obj.mesh->localAABB.transformed(model))) continue;

        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(model)));
        const VulkanContext::MeshPC pc{
            .model       = model,
            .normalCols  = {glm::vec4(nm[0], 0.0f), glm::vec4(nm[1], 0.0f), glm::vec4(nm[2], 0.0f)},
            .prevDelta   = glm::vec4(obj.motion, 0.0f),
        };

        m_context->drawMesh(obj.mesh->vbo.handle, obj.mesh->ibo.handle, obj.mesh->indexCount, pc);
        ++g_perf.sceneDraws;
    }
}

// ─── Сохранения ───────────────────────────────────────────────────────────────
void Application::refreshSaves() {
    m_saveEntries = m_saves->list();
    m_saveRefresh = 1.5f;
}

void Application::showToast(std::string text, bool error) {
    m_toast      = std::move(text);
    m_toastError = error;
    m_toastTime  = error ? 4.0f : 2.2f;
}

void Application::startedNewWorld() {
    m_currentSlot.clear();
    m_playTime      = 0.0f;
    m_autosaveTimer = 0.0f;
    char name[40];
    std::snprintf(name, sizeof name, "Мир №%04u", m_terrain.seed() % 10000u);
    m_worldName = name;
}

bool Application::saveToSlot(const std::string& slot, const std::string& name) {
    if (!m_started) return false;
    const CameraState cam{m_camera.target(), m_camera.yaw(), m_camera.pitch(), m_camera.distance()};
    const SaveGame    game = captureSave(name, static_cast<std::int64_t>(std::time(nullptr)), m_playTime, m_terrain, m_world, m_physics, cam);
    std::string       err;
    if (!m_saves->save(slot, game, err)) {
        showToast("Не удалось сохранить: " + err, true);
        return false;
    }
    refreshSaves();
    return true;
}

// "Сохранить" из меню: первый раз создаёт новый слот по времени, дальше перезаписывает его.
bool Application::saveCurrent(bool quiet) {
    if (!m_started) return false;
    if (m_currentSlot.empty()) {
        char slot[40] = "world";
        const std::time_t t = std::time(nullptr);
        if (const std::tm* tm = std::localtime(&t)) std::strftime(slot, sizeof slot, "world_%Y%m%d_%H%M%S", tm);
        m_currentSlot = slot;
    }
    if (!saveToSlot(m_currentSlot, m_worldName)) return false;
    if (!quiet) showToast("Мир сохранён: " + m_worldName);
    return true;
}

bool Application::loadFromSlot(const std::string& slot) {
    SaveGame    game;
    std::string err;
    if (!m_saves->load(slot, game, err)) {
        showToast("Не удалось загрузить: " + err, true);
        return false;
    }
    // restoreSave собирает новые объекты и подменяет живые только при успехе.
    if (!restoreSave(game, m_terrain, m_world, m_physics)) {
        showToast("Не удалось загрузить: сохранение повреждено", true);
        return false;
    }
    m_chunks->reset();  // рельеф другой (сид и правки) -- все чанки заново
    m_context->resetTemporalHistory();
    m_creatureObjects.clear();
    m_boulderObjects.clear();
    m_buildingObjects.clear();
    m_animalObjects.clear();
    m_prevAnimalPos.clear();
    m_inspectId = 0;
    m_inspectAnimal.reset();
    m_camera.restore(game.camera.target, game.camera.yaw, game.camera.pitch, game.camera.distance);
    m_cursorWorld.reset();
    m_toolDown = false;
    m_toolFirst = true;
    m_started   = true;
    m_screen    = Screen::Playing;
    m_playTime  = game.meta.playSeconds;
    m_autosaveTimer = 0.0f;
    m_worldName = game.meta.name;
    // Быстрое и авто-сохранения -- отдельные слоты; из них продолжаем "как в новом слоте", а не затираем их.
    m_currentSlot = (slot == "quick" || slot == "autosave") ? std::string() : slot;
    showToast("Загружено: " + game.meta.name);
    return true;
}

void Application::drawToast(float dt) {
    if (m_toastTime <= 0.0f) return;
    m_toastTime -= dt;
    const float a = std::clamp(std::min(m_toastTime, 0.3f) / 0.3f, 0.0f, 1.0f);  // плавное исчезновение
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float  u = screen.x * 0.01f, fs = std::max(13.0f, 1.25f * u);
    ImFont* font = m_imgui->uiFont();
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, m_toast.c_str());
    const ImVec2 p0((screen.x - ts.x) * 0.5f - 1.6f * u, 2.2f * u), p1(p0.x + ts.x + 3.2f * u, p0.y + ts.y + 1.4f * u);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const unsigned accent = m_toastError ? 0xff9a6b : style::kGlow;
    for (int g = 3; g >= 1; --g)
        dl->AddRect({p0.x - 0.2f * u * g, p0.y - 0.2f * u * g}, {p1.x + 0.2f * u * g, p1.y + 0.2f * u * g}, style::rgba(accent, 0.10f * a), 0.8f * u + 0.2f * u * g, 0, 1.5f);
    dl->AddRectFilled(p0, p1, style::rgba(style::kRightBg, 0.94f * a), 0.8f * u);
    dl->AddRect(p0, p1, style::rgba(accent, 0.55f * a), 0.8f * u);
    dl->AddText(font, fs, {p0.x + 1.6f * u, p0.y + 0.7f * u}, style::rgba(m_toastError ? 0xffd0bd : style::kText, a), m_toast.c_str());
}

// ─── pickTerrain ──────────────────────────────────────────────────────────────
// Луч из камеры через пиксель: шагаем вдоль него, пока не окажемся под рельефом,
// затем бисекцией уточняем точку. Направление берём по двум точкам внутри
// frustum (z=0.25 и 0.75): верно и для NDC глубины [0,1], и для [-1,1].
std::optional<glm::vec3> Application::pickTerrain(float mouseX, float mouseY) const {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0.0f || screen.y <= 0.0f || mouseX < -1.0e5f) return std::nullopt;
    const CameraData cam = m_camera.getData();
    const glm::mat4  inv = glm::inverse(cam.proj * cam.view);
    const float nx = 2.0f * mouseX / screen.x - 1.0f, ny = 2.0f * mouseY / screen.y - 1.0f;
    auto unproject = [&](float z) {
        const glm::vec4 p = inv * glm::vec4(nx, ny, z, 1.0f);
        return glm::vec3(p) / p.w;
    };
    const glm::vec3 origin = glm::vec3(cam.position);
    const glm::vec3 dir    = glm::normalize(unproject(0.75f) - unproject(0.25f));

    auto above = [&](float t) {
        const glm::vec3 p = origin + dir * t;
        return p.y - m_terrain.heightAt(p.x, p.z);
    };
    float t = 0.0f, prev = 0.0f;
    if (above(0.0f) <= 0.0f) return std::nullopt;  // камера под землёй
    for (int i = 0; i < 4000 && t < 3500.0f; ++i) {
        prev = t;
        t += std::max(0.5f, t * 0.01f);
        if (above(t) <= 0.0f) {
            float lo = prev, hi = t;
            for (int k = 0; k < 24; ++k) {
                const float mid = 0.5f * (lo + hi);
                (above(mid) > 0.0f ? lo : hi) = mid;
            }
            const glm::vec3 hit = origin + dir * (0.5f * (lo + hi));
            return glm::vec3(hit.x, m_terrain.heightAt(hit.x, hit.z), hit.z);
        }
    }
    return std::nullopt;
}

// ─── followTerrain ────────────────────────────────────────────────────────────
// Рельеф можно поднимать, а камера орбитирует вокруг точки обзора: без этого
// гора вырастала вокруг камеры. Высота точки плавно ведётся за землёй под ней
// (водой тоже: ниже уровня моря не уходим), а затем камеру и весь отрезок от
// неё до точки обзора мгновенно поднимаем над рельефом -- иначе хребет между
// ними закрывал бы обзор изнутри.
void Application::followTerrain(float dt) {
    constexpr float kClearance = 1.5f;
    auto ground = [&](float x, float z) { return std::max(m_terrain.heightAt(x, z), Terrain::kSeaLevel); };

    glm::vec3 t = m_camera.target();
    t.y += (ground(t.x, t.z) - t.y) * (1.0f - std::exp(-std::min(dt, 0.1f) * 6.0f));

    const glm::vec3 cam = t - m_camera.front() * m_camera.distance();
    float lift = 0.0f;
    constexpr int kSamples = 16;
    for (int i = 0; i <= kSamples; ++i) {
        const glm::vec3 p = glm::mix(t, cam, static_cast<float>(i) / kSamples);
        lift = std::max(lift, ground(p.x, p.z) + kClearance - p.y);
    }
    m_camera.setTargetY(t.y + lift);
}

// ─── stepPhysics ──────────────────────────────────────────────────────────────
// Валуны: шаг физики, затем последствия -- удар вмятывает кратер и давит существ,
// катящийся валун давит всех на пути. Всё идёт через GodContext, как у сил бога.
void Application::stepPhysics(float dt) {
    m_physics.step(dt, m_terrain);

    GodContext god(m_world, m_terrain, &m_physics, m_sound.get());
    for (const ImpactEvent& hit : m_physics.impacts()) {
        m_sound->impact(hit);
        god.erase(hit.pos.x, hit.pos.z, hit.radius * 1.8f);
        god.eraseBuildings(hit.pos.x, hit.pos.z, hit.radius + 2.8f);
        god.raise(hit.pos.x, hit.pos.z, hit.radius * 2.4f, -std::clamp(hit.impulse * 0.012f, 0.3f, 2.5f));
    }
    for (const RigidBody& b : m_physics.bodies())
        if (!b.asleep && glm::length(b.vel) > 2.0f) {
            god.erase(b.pos.x, b.pos.z, b.radius * 1.1f);
            god.eraseBuildings(b.pos.x, b.pos.z, b.radius + 2.2f);
        }
    for (const auto& rect : god.takeDirty()) m_chunks->invalidate(rect);

    // Опасности для существ: катящиеся валуны и свежие места ударов -- от них бегут.
    for (auto& ih : m_impactHazards) ih.ttl -= dt;
    std::erase_if(m_impactHazards, [](const TimedHazard& ih) { return ih.ttl <= 0.0f; });
    for (const ImpactEvent& hit : m_physics.impacts()) m_impactHazards.push_back({{hit.pos, hit.radius * 2.0f + 3.0f}, 2.5f});
    std::vector<Hazard> hazards;
    for (const RigidBody& b : m_physics.bodies())
        if (!b.asleep && glm::length(b.vel) > 1.5f) hazards.push_back({b.pos, b.radius + 1.5f});
    for (const auto& ih : m_impactHazards) hazards.push_back(ih.hazard);
    m_world.setHazards(hazards);

    m_boulderObjects.clear();
    for (const RigidBody& b : m_physics.bodies()) {
        Transform t;
        t.position    = b.pos;
        t.scale       = glm::vec3(b.radius);
        t.orientation = b.orient;
        t.useQuat     = true;
        m_boulderObjects.push_back({m_boulderMesh, t, /*solid=*/false});
        m_boulderObjects.back().trackId = m_boulderObjects.size();  // тела без id: по порядку (при удалении тела один кадр может мазнуть -- отсекается порогом)
    }
}

// ─── drawAnimalInspector ──────────────────────────────────────────────────────
// Панель зверя: вид и рацион, чем занят, сытость, здоровье, возраст; у хищника -- за кем гонится.
void Application::drawAnimalInspector(const CameraData& cam) {
    const Animal* an = nullptr;
    for (const Animal& a : m_world.fauna().animals())
        if (a.id == *m_inspectAnimal) { an = &a; break; }
    if (!an) { m_inspectAnimal.reset(); return; }  // зверя больше нет -- панель закрывается
    const SpeciesDef& d = speciesDef(an->species);

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float  u = screen.x * 0.01f;
    ImDrawList*  dl = ImGui::GetForegroundDrawList();
    ImFont*      font = m_imgui->uiFont();
    {
        const glm::vec4 c = cam.proj * cam.view * glm::vec4(an->position.x, m_terrain.heightAt(an->position.x, an->position.z) + 0.6f * d.size, an->position.z, 1.0f);
        if (c.w > 0.0f) {
            const ImVec2 p((c.x / c.w * 0.5f + 0.5f) * screen.x, (c.y / c.w * 0.5f + 0.5f) * screen.y);
            const float  r = std::clamp(2400.0f * d.size / std::max(c.w, 1.0f), 12.0f, 70.0f);
            dl->AddCircle(p, r, style::rgba(style::kGlow, 0.35f), 40, 5.0f);
            dl->AddCircle(p, r, style::rgba(style::kMint, 0.95f), 40, 2.0f);
        }
    }
    const float W = 27.0f * u, x0 = screen.x - W - 2.0f * u, y0 = 5.0f * u, pad = 1.4f * u;
    const float fsTitle = std::max(15.0f, 1.55f * u), fs = std::max(11.5f, 1.05f * u), rowH = 2.1f * u;
    const float H = pad * 2 + fsTitle * 1.3f + fs * 1.5f + rowH * 2 + fs * 1.5f;
    for (int g = 3; g >= 1; --g)
        dl->AddRect({x0 - 0.2f * u * g, y0 - 0.2f * u * g}, {x0 + W + 0.2f * u * g, y0 + H + 0.2f * u * g}, style::rgba(style::kGlow, 0.09f * (1.0f - 0.25f * (g - 1))), 1.0f * u + 0.2f * u * g, 0, 1.5f);
    dl->AddRectFilled({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kRightBg, 0.93f), 1.0f * u);
    dl->AddRect({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kGlow, 0.4f), 1.0f * u);

    char  buf[128];
    float y = y0 + pad;
    dl->AddText(font, fsTitle, {x0 + pad, y}, style::rgba(style::kText), d.name);
    y += fsTitle * 1.3f;
    std::snprintf(buf, sizeof buf, "%s · %s", d.diet == Diet::Predator ? "Хищник" : "Травоядное", animalStateName(an->state));
    dl->AddText(font, fs * 1.1f, {x0 + pad, y}, style::rgba(style::kMint), buf);
    y += fs * 1.5f;
    struct Bar { const char* label; float v; unsigned color; };
    const Bar bars[] = {{"Здоровье", an->health, 0x7dffc4}, {"Голод", an->hunger, 0xffb86b}};
    for (const Bar& b : bars) {
        dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), b.label);
        const float bx = x0 + pad + 9.5f * u, bw = W - pad * 2 - 9.5f * u, by = y + fs * 0.5f, th = std::max(3.0f, 0.38f * u);
        dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw, by + th * 0.5f}, style::rgba(0x12413b), th * 0.5f);
        dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw * std::clamp(b.v, 0.0f, 1.0f), by + th * 0.5f}, style::rgba(b.color), th * 0.5f);
        y += rowH;
    }
    std::snprintf(buf, sizeof buf, "%s · возраст %.0f%% жизни", an->age >= d.maturity ? "взрослый" : "детёныш", 100.0f * an->age / d.lifespan);
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
}

// ─── drawBushInspector ────────────────────────────────────────────────────────
// Панель куста: вид ягод, сколько их осталось, сытность и скорость отрастания относительно малины.
void Application::drawBushInspector(const CameraData& cam) {
    const FoodSource* bush = nullptr;
    for (const FoodSource& f : m_world.food())
        if (std::hypot(f.position.x - m_inspectBush->x, f.position.z - m_inspectBush->y) < 0.2f) { bush = &f; break; }
    if (!bush) { m_inspectBush.reset(); return; }  // куст убрали -- панель закрывается

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float  u = screen.x * 0.01f;
    ImDrawList*  dl = ImGui::GetForegroundDrawList();
    ImFont*      font = m_imgui->uiFont();
    const BerryDef& def = berryDef(bush->kind);

    {
        const glm::vec4 c = cam.proj * cam.view * glm::vec4(bush->position.x, m_terrain.heightAt(bush->position.x, bush->position.z) + 0.5f, bush->position.z, 1.0f);
        if (c.w > 0.0f) {
            const ImVec2 p((c.x / c.w * 0.5f + 0.5f) * screen.x, (c.y / c.w * 0.5f + 0.5f) * screen.y);
            const float  r = std::clamp(1800.0f / std::max(c.w, 1.0f), 10.0f, 50.0f);
            dl->AddCircle(p, r, style::rgba(style::kGlow, 0.35f), 40, 5.0f);
            dl->AddCircle(p, r, style::rgba(style::kMint, 0.95f), 40, 2.0f);
        }
    }
    const float W = 27.0f * u, x0 = screen.x - W - 2.0f * u, y0 = 5.0f * u, pad = 1.4f * u;
    const float fsTitle = std::max(15.0f, 1.55f * u), fs = std::max(11.5f, 1.05f * u), rowH = 2.1f * u;
    const float H = pad * 2 + fsTitle * 1.3f + fs * 1.5f + rowH + fs * 3.0f;
    for (int g = 3; g >= 1; --g)
        dl->AddRect({x0 - 0.2f * u * g, y0 - 0.2f * u * g}, {x0 + W + 0.2f * u * g, y0 + H + 0.2f * u * g}, style::rgba(style::kGlow, 0.09f * (1.0f - 0.25f * (g - 1))), 1.0f * u + 0.2f * u * g, 0, 1.5f);
    dl->AddRectFilled({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kRightBg, 0.93f), 1.0f * u);
    dl->AddRect({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kGlow, 0.4f), 1.0f * u);

    char  buf[128];
    float y = y0 + pad;
    dl->AddText(font, fsTitle, {x0 + pad, y}, style::rgba(style::kText), def.name);
    // Цветная точка вида рядом с названием
    const ImU32 berryCol = IM_COL32(static_cast<int>(def.berryColor.r * 255), static_cast<int>(def.berryColor.g * 255), static_cast<int>(def.berryColor.b * 255), 255);
    dl->AddCircleFilled({x0 + W - pad - 0.9f * u, y + fsTitle * 0.55f}, 0.9f * u, berryCol);
    y += fsTitle * 1.3f;
    dl->AddText(font, fs * 1.1f, {x0 + pad, y}, style::rgba(style::kMint), "Ягодный куст");
    y += fs * 1.5f;
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), "Ягод");
    const float bx = x0 + pad + 9.5f * u, bw = W - pad * 2 - 9.5f * u, by = y + fs * 0.5f, th = std::max(3.0f, 0.38f * u);
    dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw, by + th * 0.5f}, style::rgba(0x12413b), th * 0.5f);
    dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw * std::clamp(bush->amount, 0.0f, 1.0f), by + th * 0.5f}, berryCol, th * 0.5f);
    y += rowH;
    std::snprintf(buf, sizeof buf, "Сытность ×%.2f · отрастание ×%.2f", def.nutrition, def.regrow);
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
    y += fs * 1.4f;
    // Действие на едока (подсказка богу: существа различают ягоды только по внешнему виду).
    std::string effects;
    auto add = [&](const char* s) { effects += effects.empty() ? "" : " · "; effects += s; };
    if (def.lethal > 0.0f) add("смертельна");
    if (def.vomit > 0.0f) add("рвота");
    if (def.diarrhea > 0.0f) add("понос");
    if (def.hallucination > 0.0f) add("галлюцинации");
    if (effects.empty()) effects = "безопасна";
    std::snprintf(buf, sizeof buf, "%s", effects.c_str());
    dl->AddText(font, fs, {x0 + pad, y}, effects == "безопасна" ? style::rgba(style::kDim) : style::rgba(0xa6e06b), buf);
}

// ─── drawInspector ────────────────────────────────────────────────────────────
void Application::drawInspector(const CameraData& cam) {
    if (m_inspectId == 0) {
        if (m_inspectAnimal) drawAnimalInspector(cam);
        else if (m_inspectBush) drawBushInspector(cam);
        return;
    }
    const Creature* cr = nullptr;
    for (const Inhabitant& in : m_world.population())
        if (in.creature.id == m_inspectId) { cr = &in.creature; break; }
    if (!cr) { m_inspectId = 0; return; }  // умерло -- панель закрывается

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float  u = screen.x * 0.01f;
    ImDrawList*  dl = ImGui::GetForegroundDrawList();
    ImFont*      font = m_imgui->uiFont();

    // Кольцо вокруг выбранного существа.
    {
        const glm::vec4 c = cam.proj * cam.view * glm::vec4(cr->position.x, cr->position.y + 0.6f, cr->position.z, 1.0f);
        if (c.w > 0.0f) {
            const ImVec2 p((c.x / c.w * 0.5f + 0.5f) * screen.x, (c.y / c.w * 0.5f + 0.5f) * screen.y);
            const float  r = std::clamp(2200.0f / std::max(c.w, 1.0f), 12.0f, 60.0f);
            dl->AddCircle(p, r, style::rgba(style::kGlow, 0.35f), 48, 6.0f);
            dl->AddCircle(p, r, style::rgba(style::kMint, 0.95f), 48, 2.0f);
        }
    }

    // Панель справа вверху.
    const float W = 27.0f * u, x0 = screen.x - W - 2.0f * u, y0 = 5.0f * u, pad = 1.4f * u;
    const float fsTitle = std::max(15.0f, 1.55f * u), fs = std::max(11.5f, 1.05f * u), rowH = 2.1f * u;
    const auto lifeFrac = cr->age / cr->lifespan();
    struct Bar { const char* label; float v; unsigned color; bool inverted; };
    const Bar bars[] = {
        {"Здоровье", cr->health, 0x7dffc4, false},
        {"Голод", cr->needs.hunger, 0xffb86b, true},
        {"Энергия", cr->needs.energy, 0x6bd6ff, false},
        {"Страх", cr->needs.fear, 0xff7b6b, true},
        {"Тяга к паре", cr->needs.libido, 0xff8fd0, false},
        {"Общение", cr->needs.social, 0xb7a6ff, false},
        {"Яд", cr->toxin, 0xa6e06b, true},
    };
    const float H = pad * 2 + fsTitle * 1.3f + fs * 1.5f + rowH * 7 + fs * (7.55f + (cr->grudge > 0.3f ? 1.35f : 0.0f));
    for (int g = 3; g >= 1; --g)
        dl->AddRect({x0 - 0.2f * u * g, y0 - 0.2f * u * g}, {x0 + W + 0.2f * u * g, y0 + H + 0.2f * u * g}, style::rgba(style::kGlow, 0.09f * (1.0f - 0.25f * (g - 1))), 1.0f * u + 0.2f * u * g, 0, 1.5f);
    dl->AddRectFilled({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kRightBg, 0.93f), 1.0f * u);
    dl->AddRect({x0, y0}, {x0 + W, y0 + H}, style::rgba(style::kGlow, 0.4f), 1.0f * u);

    char buf[128];
    float y = y0 + pad;
    std::snprintf(buf, sizeof buf, "Существо №%llu", static_cast<unsigned long long>(cr->id));
    dl->AddText(font, fsTitle, {x0 + pad, y}, style::rgba(style::kText), buf);
    y += fsTitle * 1.3f;
    dl->AddText(font, fs * 1.1f, {x0 + pad, y}, style::rgba(style::kMint), behaviorName(cr->behavior));
    y += fs * 1.5f;
    for (const Bar& b : bars) {
        dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), b.label);
        const float bx = x0 + pad + 9.5f * u, bw = W - pad * 2 - 9.5f * u, by = y + fs * 0.5f, th = std::max(3.0f, 0.38f * u);
        dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw, by + th * 0.5f}, style::rgba(0x12413b), th * 0.5f);
        const float v = std::clamp(b.v, 0.0f, 1.0f);
        dl->AddRectFilled({bx, by - th * 0.5f}, {bx + bw * v, by + th * 0.5f}, style::rgba(b.color), th * 0.5f);
        y += rowH;
    }
    std::snprintf(buf, sizeof buf, "Возраст %.0f%% жизни · %s", lifeFrac * 100.0f, cr->isMature() ? "взрослый" : "молодой");
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
    y += fs * 1.35f;
    std::snprintf(buf, sizeof buf, "Размер %.1f · скорость %.1f · детей %zu", cr->size(), cr->maxSpeed(), cr->childIds.size());
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
    y += fs * 1.35f;
    std::snprintf(buf, sizeof buf, "Храбрость %.0f%% · общительность %.0f%%", cr->boldness() * 100.0f, cr->sociability() * 100.0f);
    dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
    y += fs * 1.35f;
    {   // Ресурсы и орудия.
        std::string kit = "Брёвен " + std::to_string(static_cast<int>(cr->wood));
        if (cr->hasSpear()) kit += " · копьё";
        if (cr->hasShield()) kit += " · щит";
        if (cr->knows(kSkillSpear)) kit += cr->knows(kSkillShield) ? " · умеет: копьё, щит" : " · умеет: копьё";
        dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), kit.c_str());
        y += fs * 1.35f;
    }
    // Что помнит о ягодах: сколько видов пробовало, какую любит больше всего и какую обходит.
    {
        // Недомогания: то, что сейчас с существом (по их уровням).
        std::string status;
        auto add = [&](const char* s) { status += status.empty() ? "" : ", "; status += s; };
        if (cr->toxin > 0.05f) add("яд");
        if (cr->diarrhea > 0.05f) add("понос");
        if (cr->vomitStun > 0.0f) add("рвота");
        else if (cr->nausea > 0.3f) add("тошнит");
        if (cr->hallucination > 0.05f) add("галлюцинации");
        std::snprintf(buf, sizeof buf, "Самочувствие: %s", status.empty() ? "в порядке" : status.c_str());
        dl->AddText(font, fs, {x0 + pad, y}, status.empty() ? style::rgba(style::kDim) : style::rgba(0xa6e06b), buf);
        y += fs * 1.35f;
        if (cr->grudge > 0.3f) {
            std::snprintf(buf, sizeof buf, "Зуб на №%llu (%.0f%%)%s", static_cast<unsigned long long>(cr->grudgeId), cr->grudge * 100.0f,
                          cr->carryAmount > 0.0f ? " · несёт ягоды" : "");
            dl->AddText(font, fs, {x0 + pad, y}, style::rgba(0xff8f6b), buf);
            y += fs * 1.35f;
        }
        // Память о вкусе (по внешнему виду ягод): сколько групп пробовало, какие любит и какие обходит.
        int known = 0, best = -1, worst = -1;
        for (int k = 0; k < kBerryLookCount; ++k) {
            if (!cr->knowsLook(static_cast<BerryLook>(k))) continue;
            ++known;
            if (best < 0 || cr->taste[static_cast<std::size_t>(k)] > cr->taste[static_cast<std::size_t>(best)]) best = k;
            if (worst < 0 || cr->taste[static_cast<std::size_t>(k)] < cr->taste[static_cast<std::size_t>(worst)]) worst = k;
        }
        if (known == 0) std::snprintf(buf, sizeof buf, "Ягод ещё не пробовало");
        else if (cr->taste[static_cast<std::size_t>(worst)] < Creature::kMinAppeal)
            std::snprintf(buf, sizeof buf, "Любит %s · обходит %s", lookName(static_cast<BerryLook>(best)), lookName(static_cast<BerryLook>(worst)));
        else std::snprintf(buf, sizeof buf, "Любит %s ягоды", lookName(static_cast<BerryLook>(best)));
        dl->AddText(font, fs, {x0 + pad, y}, style::rgba(style::kDim), buf);
    }
}

// ─── applyPower ───────────────────────────────────────────────────────────────
void Application::applyPower(float dt) {
    m_eraseSoundTimer -= dt;
    if (!m_toolDown) { m_sound->terrainRumble(0.0f); return; }
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const auto   hit = pickTerrain(mp.x, mp.y);
    if (!hit) { m_sound->terrainRumble(0.0f); return; }
    const auto powers = m_powers.all();
    const PowerDef& power = powers[static_cast<std::size_t>(std::min(m_power, static_cast<int>(powers.size()) - 1))];

    // Гул, пока лепят рельеф кистью.
    m_sound->terrainRumble((power.id == "raise" || power.id == "lower" || power.id == "flatten") ? 1.0f : 0.0f);

    bool fire = false;
    if (m_toolFirst) {
        fire = true;
    } else if (power.interval == 0.0f) {
        fire = true;
    } else if (power.interval > 0.0f) {
        m_toolTimer -= dt;
        fire = m_toolTimer <= 0.0f;
    }
    if (!fire) return;
    if (power.interval > 0.0f) m_toolTimer = power.interval;

    // "Осмотр": выбрать ближайшее к точке клика существо (в радиусе кисти), панель покажет его состояние;
    // если существа рядом нет -- ближайший ягодный куст.
    if (power.id == "inspect") {
        float best = std::max(m_brush, 2.5f);
        m_inspectId = 0;
        m_inspectBush.reset();
        m_inspectAnimal.reset();
        for (const Inhabitant& in : m_world.population()) {
            const float d = std::hypot(in.creature.position.x - hit->x, in.creature.position.z - hit->z);
            if (d < best) { best = d; m_inspectId = in.creature.id; }
        }
        // Зверь ближе существа -- осматриваем его (звери крупнее и чаще оказываются под курсором).
        std::size_t beast = 0;
        if (m_world.fauna().nearest(hit->x, hit->z, std::max(m_brush, 2.5f), beast)) {
            const Animal& a = m_world.fauna().animals()[beast];
            if (std::hypot(a.position.x - hit->x, a.position.z - hit->z) < best || m_inspectId == 0) {
                m_inspectId = 0;
                m_inspectAnimal = a.id;
            }
        }
        std::size_t bush = 0;
        if (m_inspectId == 0 && !m_inspectAnimal && m_world.nearestBush(hit->x, hit->z, std::max(m_brush, 2.5f), bush))
            m_inspectBush = glm::vec2(m_world.food()[bush].position.x, m_world.food()[bush].position.z);
        m_toolFirst = false;
        return;
    }

    GodContext god(m_world, m_terrain, &m_physics, m_sound.get());
    const std::size_t housesBefore = m_world.buildings().size(), bushesBefore = m_world.foodCount();
    power.apply(god, PowerCall{hit->x, hit->z, m_brush, dt, m_toolFirst});
    m_toolFirst = false;
    for (const auto& rect : god.takeDirty()) m_chunks->invalidate(rect);

    // Звуки встроенных сил: стройка и спавн -- по факту, стирание -- на каждое применение.
    const glm::vec3 at{hit->x, hit->y, hit->z};
    if (power.id == "spawn") m_sound->effect(audio::SoundId::Pop, at, 0.8f, 0.9f + 0.002f * static_cast<float>(m_world.populationCount() % 100));
    else if (power.id == "erase") {
        if (m_eraseSoundTimer <= 0.0f) { m_sound->effect(audio::SoundId::Whoosh, at, 0.7f); m_eraseSoundTimer = 0.4f; }  // не чаще раза в 0.4 с
    }
    else if (power.id == "house" && m_world.buildings().size() > housesBefore) m_sound->effect(audio::SoundId::Knock, at, 0.9f);
    else if (power.id == "bush" && m_world.foodCount() > bushesBefore) m_sound->effect(audio::SoundId::Pop, at, 0.45f, 1.3f);
}

// ─── drawBrushRing ────────────────────────────────────────────────────────────
// Кольцо кисти по рельефу: точки круга ложатся на высоту земли и проецируются в экран.
void Application::drawBrushRing(const CameraData& cam) {
    if (!m_cursorWorld) return;
    const ImVec2    screen = ImGui::GetIO().DisplaySize;
    const glm::mat4 vp     = cam.proj * cam.view;
    ImDrawList*     dl     = ImGui::GetForegroundDrawList();
    constexpr int   kSegs  = 64;
    std::vector<ImVec2> pts;
    pts.reserve(kSegs + 1);
    for (int i = 0; i <= kSegs; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / kSegs;
        const float x = m_cursorWorld->x + std::cos(a) * m_brush, z = m_cursorWorld->z + std::sin(a) * m_brush;
        const glm::vec4 c = vp * glm::vec4(x, m_terrain.heightAt(x, z) + 0.15f, z, 1.0f);
        if (c.w <= 0.0f) return;
        pts.emplace_back((c.x / c.w * 0.5f + 0.5f) * screen.x, (c.y / c.w * 0.5f + 0.5f) * screen.y);
    }
    // Те же sRGB->linear цвета, что у меню (swapchain SRGB).
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), style::rgba(style::kGlow, 0.25f), ImDrawFlags_None, 6.0f);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), style::rgba(style::kMint, 0.95f), ImDrawFlags_None, 2.0f);
    const glm::vec4 cc = vp * glm::vec4(m_cursorWorld->x, m_cursorWorld->y + 0.15f, m_cursorWorld->z, 1.0f);
    if (cc.w > 0.0f) {
        const ImVec2 c((cc.x / cc.w * 0.5f + 0.5f) * screen.x, (cc.y / cc.w * 0.5f + 0.5f) * screen.y);
        dl->AddCircleFilled(c, 3.0f, style::rgba(style::kMint, 0.95f));
    }
}

// ─── syncModRows ──────────────────────────────────────────────────────────────
void Application::syncModRows(std::vector<ModRow>& rows) const {
    for (const ModInfo& m : m_mods->mods()) {
        ModRow r;
        r.name        = m.name;
        r.description = m.description;
        r.enabled     = m.enabled;
        r.canToggle   = m.valid;
        r.meta        = "v" + m.version + (m.author.empty() ? "" : " · " + m.author) + " · сил: " + std::to_string(m.powerCount);
        if (!m.errors->empty()) r.error = m.errors->front() + (m.errors->size() > 1 ? "  (+" + std::to_string(m.errors->size() - 1) + ")" : "");
        rows.push_back(std::move(r));
    }
}

// ─── applySettings ────────────────────────────────────────────────────────────
void Application::applySettings() {
    m_settings.clamp();
    if (m_settings == m_applied) return;
    m_context->setVsync(m_settings.vsync);
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    switch (m_settings.msaa) {
        case 2:  samples = VK_SAMPLE_COUNT_2_BIT; break;
        case 4:  samples = VK_SAMPLE_COUNT_4_BIT; break;
        case 8:  samples = VK_SAMPLE_COUNT_8_BIT; break;
        default: break;
    }
    m_context->setMsaa(samples);  // сам урежет до возможностей GPU

    // Апскейлер пересоздаёт таргеты: пока тянут ползунок масштаба, не дёргаем -- применится при отпускании.
    const bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const float scale   = dragging ? m_applied.renderScale : m_settings.renderScale;
    const int   mode    = dragging ? m_applied.upscaler : m_settings.upscaler;
    m_context->setUpscaler(static_cast<VulkanContext::Upscaler>(mode), mode == 0 ? 1.0f : scale);
    m_context->setSharpness(m_settings.sharpness);
    m_context->setFrameGeneration(m_settings.frameGen && mode == 3);
    m_audio->setVolumes(m_settings.volMaster, m_settings.volAmbient, m_settings.volCreatures, m_settings.volEffects);
    ImGui::GetStyle().FontScaleMain = m_settings.uiScale;
    m_applied = m_settings;
    if (dragging) { m_applied.upscaler = mode; m_applied.renderScale = scale; }  // ещё не применено -- сработает позже
}

// ─── updateAutoViewRadius ─────────────────────────────────────────────────────
// Подбор дальности под железо: если FPS устойчиво ниже ~54 -- радиус быстро
// падает и запоминается как потолок; если устойчиво выше ~57 -- растёт по
// чанку раз в несколько секунд, пока не упрётся в потолок. Потолок "забывается"
// через 45 с, чтобы попробовать ещё раз (например, ушла нагрузка). Кадры, в
// которые строились чанки, не считаются -- стройка сама роняет FPS.
void Application::updateAutoViewRadius(float dt) {
    if (!m_settings.autoViewRadius || dt <= 0.0f) return;
    dt = std::min(dt, 0.25f);
    if (m_chunks->buildsLastUpdate() > 0) { m_overTime = m_underTime = 0.0f; return; }

    m_frameAvg = m_frameAvg <= 0.0f ? dt : m_frameAvg + (dt - m_frameAvg) * 0.05f;
    if (m_frameAvg > 1.0f / 54.0f)      { m_overTime += dt; m_underTime = 0.0f; }
    else if (m_frameAvg < 1.0f / 57.0f) { m_underTime += dt; m_overTime = 0.0f; }
    else                                { m_overTime = m_underTime = 0.0f; }

    m_ceilingAge += dt;
    if (m_ceilingAge > 45.0f) { m_ceiling = 64.0f; m_ceilingAge = 0.0f; }

    const float cap = std::min(static_cast<float>(m_settings.viewRadius), m_ceiling);
    if (m_overTime > 1.0f) {
        m_autoRadius = std::max(4.0f, std::floor(m_autoRadius * 0.85f));
        m_ceiling    = m_autoRadius;
        m_ceilingAge = 0.0f;
        m_overTime   = 0.0f;
    } else if (m_underTime > 4.0f && m_autoRadius < cap) {
        m_autoRadius += 1.0f;
        m_underTime = 0.0f;
    }
}

// ─── drawShadowCasters ────────────────────────────────────────────────────────
void Application::drawShadowCasters(const std::vector<SceneObject>& objects, const Frustum& lightFrustum, float minTop) {
    for (const auto& obj : objects) {
        if (!obj.mesh) continue;
        const glm::mat4 model = obj.transform.matrix();
        const AABB      wb    = obj.mesh->localAABB.transformed(model);
        if (wb.max.y < minTop || !lightFrustum.intersects(wb)) continue;
        m_context->drawShadowMesh(obj.mesh->vbo.handle, obj.mesh->ibo.handle, obj.mesh->indexCount, model);
        ++g_perf.shadowDraws;
        ++g_perf.cascadeDraws[g_perf.cascade];
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
    {
        const Climate cl = withAltitude(m_terrain.climateAt(t.x, t.z), t.y);
        ImGui::Text("Biome:    %s (T %.2f, M %.2f)", biomeName(classify(cl)), cl.temperature, cl.moisture);
    }
    ImGui::Text("Yaw/Pitch: %.1f / %.1f", m_camera.yaw(), m_camera.pitch());
    ImGui::Text("Distance: %.1f", m_camera.distance());
    ImGui::Separator();
    ImGui::Text("Chunks: %zu, radius %d%s", m_chunks->chunkCount(), m_chunks->viewRadius(), m_settings.autoViewRadius ? " (auto)" : "");
    ImGui::Text("Population: %zu", m_world.populationCount());
    {
        const WorldStats& st = m_world.stats();
        ImGui::Text("Births %llu  Deaths: old %llu, starved %llu, poison %llu, other %llu", static_cast<unsigned long long>(st.births),
                    static_cast<unsigned long long>(st.deathsOld), static_cast<unsigned long long>(st.deathsStarved),
                    static_cast<unsigned long long>(st.deathsPoison), static_cast<unsigned long long>(st.deathsExhausted + st.deathsOther));
    }
    ImGui::Text("Food sources: %zu (awake cells %zu)", m_world.foodCount(), m_world.loadedBushCells());
    {
        std::size_t herb = 0, pred = 0;
        for (const Animal& a : m_world.fauna().animals()) (isPredator(a.species) ? pred : herb)++;
        const FaunaStats& fs = m_world.fauna().stats();
        ImGui::Text("Trees felled %llu, spears %llu, shields %llu, stabs %llu, blocks %llu, learned %llu", static_cast<unsigned long long>(m_world.stats().treesFelled),
                    static_cast<unsigned long long>(m_world.stats().spearsMade), static_cast<unsigned long long>(m_world.stats().shieldsMade),
                    static_cast<unsigned long long>(m_world.stats().spearHits), static_cast<unsigned long long>(m_world.stats().shieldBlocks),
                    static_cast<unsigned long long>(m_world.stats().skillsLearned));
        ImGui::Text("Animals: %zu (herbivores %zu, predators %zu), kills %llu, creatures bitten %llu, mauled %llu", m_world.fauna().count(), herb, pred,
                    static_cast<unsigned long long>(fs.kills), static_cast<unsigned long long>(fs.creatureBites),
                    static_cast<unsigned long long>(m_world.stats().deathsPredation));
    }
    ImGui::Separator();
    ImGui::TextUnformatted("WASD pan, RMB-drag orbit, wheel zoom, Esc menu, F10 quit");
    ImGui::TextUnformatted("LMB use power, 1-9/0 select, [ ] or Ctrl+wheel brush size");
    ImGui::TextUnformatted("F toggle lantern");
    ImGui::End();
}

}  // namespace wsim
