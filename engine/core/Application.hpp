#pragma once

#include "core/SoundDirector.hpp"
#include "mods/ModManager.hpp"
#include "renderer/Camera.hpp"
#include "renderer/Lights.hpp"
#include "renderer/SceneObject.hpp"
#include "sim/Physics.hpp"
#include "sim/SaveGame.hpp"
#include "sim/Powers.hpp"
#include "sim/Terrain.hpp"
#include "sim/World.hpp"
#include "ui/ImGuiLayer.hpp"
#include "ui/MainMenu.hpp"
#include "ui/PowerBar.hpp"
#include "ui/Settings.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wsim {

class Window;
class VulkanContext;
class InputSystem;
class TerrainChunks;
struct Frustum;

struct AppConfig {
    std::string_view title     = "WorldSimulator";
    uint32_t         width     = 1920;
    uint32_t         height    = 1080;
    bool             vsync     = true;
    std::string_view shaderDir;
    std::string_view assetDir;
    std::string_view modsDir;  // папка с модами рядом с игрой (плюс пользовательская в ~/.local/share)
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
    static void trackMotion(std::vector<SceneObject>& objects, std::unordered_map<std::uint64_t, glm::vec3>& prev);
    // minTop: объекты, чья верхушка ниже этой высоты, в этот каскад не попадают (см. render()).
    void drawShadowCasters(const std::vector<SceneObject>& objects, const Frustum& lightFrustum, float minTop = -1e9f);

    AppConfig                      m_config;
    std::unique_ptr<Window>        m_window;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<InputSystem>   m_input;
    std::unique_ptr<ImGuiLayer>    m_imgui;

    // Главное меню поверх игры: пока открыто, камера и отрисовка сцены
    // отключены, а настоящий m_world стоит на паузе. Колония в чашке меню --
    // отдельный декоративный m_menuWorld (иначе настоящий мир за время в
    // меню разросся бы до kMaxPopulation, голод пока никого не убивает).
    // Esc переключает меню из игры и обратно, как только игра уже начата.
    enum class Screen { MainMenu, Playing };
    Screen                    m_screen  = Screen::MainMenu;
    bool                      m_started = false;
    std::unique_ptr<MainMenu> m_menu;

    OrbitCamera               m_camera;
    LightsData                m_lights;

    // Симуляция + её визуальное представление. World ничего не знает про
    // Vulkan (см. sim/World.hpp) -- Application только читает population()
    // каждый update() и строит SceneObject'ы из позиций. Все существа делят
    // один и тот же m_creatureMesh (см. деструктор в .cpp -- нельзя удалять
    // меш через перебор m_creatureObjects, иначе двойной destroy()).
    World                     m_world;
    World                     m_menuWorld;
    std::shared_ptr<Mesh>     m_creatureMesh;
    std::vector<SceneObject>  m_creatureObjects;
    // Позиции объектов в прошлом нарисованном кадре (trackId → позиция): векторы движения для temporal-апскейлера
    std::unordered_map<std::uint64_t, glm::vec3> m_prevCreaturePos, m_prevBoulderPos;
    // Валуны (физика): тела живут в m_physics, их меши -- общая каменная "картошка".
    PhysicsWorld              m_physics;
    std::shared_ptr<Mesh>     m_boulderMesh;
    std::vector<SceneObject>  m_boulderObjects;
    // Постройки: несколько заранее собранных видов домов (procgen), объекты -- по списку World::buildings().
    static constexpr std::uint32_t kHouseVariants = 6;
    std::vector<std::shared_ptr<Mesh>> m_houseMeshes;
    std::vector<SceneObject>           m_buildingObjects;
    // Еда: кусты (по World::food()).
    std::array<std::shared_ptr<Mesh>, kBerryKindCount> m_bushMeshes;  // по одному на вид ягод (цвет листвы и ягод)
    // Дикие животные (World::fauna()): по мешу на вид; объекты пересобираются из мира каждый update().
    std::array<std::shared_ptr<Mesh>, kSpeciesCount>   m_animalMeshes;
    std::vector<SceneObject>                           m_animalObjects;
    std::unordered_map<std::uint64_t, glm::vec3>       m_prevAnimalPos;
    std::vector<SceneObject>           m_foodObjects;

    // Рельеф — независим от World в этом раунде (чисто визуальный/
    // географический слой, никакого геймплейного сопряжения, см. CLAUDE.md).
    // Меш строится один раз в конструкторе (в m_scene, статично); update()
    // только сэмплирует высоту для позиционирования существ на поверхности.
    Terrain                   m_terrain;
    std::unique_ptr<TerrainChunks> m_chunks;  // после m_context: деструктор освобождает GPU-меши

    // Силы бога: выбранная сила, радиус кисти и состояние удержания кнопки.
    // Кисть работает от курсора: луч через пиксель -> пересечение с рельефом.
    [[nodiscard]] std::optional<glm::vec3> pickTerrain(float mouseX, float mouseY) const;
    void applyPower(float dt);
    void stepPhysics(float dt);
    // Осмотр существа: выбор кликом (сила "Осмотр") и панель состояния со своими нуждами и занятием.
    void drawInspector(const CameraData& cam);
    void drawBushInspector(const CameraData& cam);
    void drawAnimalInspector(const CameraData& cam);
    std::uint64_t              m_inspectId = 0;
    std::optional<std::uint64_t> m_inspectAnimal;  // "Осмотр" зверя (id)
    std::optional<glm::vec2>   m_inspectBush;   // "Осмотр" куста (если рядом не нашлось существа): его положение
    struct TimedHazard { Hazard hazard; float ttl; };
    std::vector<TimedHazard>   m_impactHazards;  // места свежих ударов валунов: существа какое-то время их боятся
    void followTerrain(float dt);  // точка обзора по высоте земли, камера не лезет в рельеф
    void drawBrushRing(const CameraData& cam);
    // Звук: движок (miniaudio) и режиссёр, решающий что играть по состоянию игры.
    std::unique_ptr<audio::AudioEngine> m_audio;
    std::unique_ptr<SoundDirector>      m_sound;
    PowerRegistry              m_powers;
    std::unique_ptr<ModManager> m_mods;
    void syncModRows(std::vector<ModRow>& rows) const;
    std::unique_ptr<PowerBar>  m_powerBar;
    int                        m_power = 0;
    float                      m_brush = 10.0f;
    float                      m_eraseSoundTimer = 0.0f;
    bool                       m_toolDown = false;   // ЛКМ зажата вне интерфейса
    bool                       m_toolFirst = true;   // следующий вызов -- первый с момента нажатия
    float                      m_toolTimer = 0.0f;
    bool                       m_barHover = false;
    bool                       m_flashlight = false;  // F: фонарь -- прожектор от камеры на точку под курсором
    std::optional<glm::vec3>   m_cursorWorld;

    // Сохранения: папка с файлами .wsim, текущий слот (повторное "Сохранить" перезаписывает его),
    // время игры в этом мире и всплывающие сообщения.
    std::unique_ptr<SaveStore> m_saves;
    std::vector<SaveEntry>     m_saveEntries;     // кэш списка (обновляется раз в ~1.5 с в меню и после действий)
    float                      m_saveRefresh = 0.0f;
    std::string                m_currentSlot;     // пусто -- мир ещё не сохранялся
    std::string                m_worldName = "Мир";
    float                      m_playTime = 0.0f; // секунд игры в этом мире
    float                      m_autosaveTimer = 0.0f;
    std::string                m_toast;
    float                      m_toastTime = 0.0f;
    bool                       m_toastError = false;
    bool saveToSlot(const std::string& slot, const std::string& name);
    bool saveCurrent(bool quiet = false);         // "Сохранить" из меню / F5 использует quick-слот
    bool loadFromSlot(const std::string& slot);
    void refreshSaves();
    void showToast(std::string text, bool error = false);
    void drawToast(float dt);
    void startedNewWorld();                       // сброс слота, времени и имени под только что созданный мир

    // Настройки (меню "Настройки"): m_settings редактирует меню, applySettings()
    // переносит изменения в движок, а на диск пишем, когда мышь отпущена
    // (чтобы не сохранять на каждом кадре перетаскивания ползунка).
    void     applySettings();
    void     updateAutoViewRadius(float dt);  // подбор дальности под FPS (Settings::autoViewRadius)
    Settings m_settings;
    Settings m_applied;   // что уже применено к движку
    Settings m_saved;     // что уже лежит на диске
    std::string m_settingsPath;
    // Автодальность: сглаженное время кадра, накопленная перегрузка/запас и
    // "потолок" -- радиус, на котором уже тормозило (не лезем выше, пока не пройдёт время).
    float m_autoRadius = 12.0f;
    float m_frameAvg   = 0.0f, m_overTime = 0.0f, m_underTime = 0.0f;
    float m_ceiling    = 64.0f, m_ceilingAge = 0.0f;
};

}  // namespace wsim
