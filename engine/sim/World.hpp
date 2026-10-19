#pragma once

#include "sim/Berries.hpp"
#include "sim/Creature.hpp"
#include "sim/Fauna.hpp"
#include "sim/Structures.hpp"
#include "sim/Terrain.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wsim {

// Существо в мире + его сигналы наружу с ПРОШЛОГО тика (alarmCall/matingCall/
// dominanceDisplay). Прошлый, а не текущий тик -- иначе соседи в одном
// vector<Inhabitant> сенсили бы друг друга в порядке, зависящем от индекса
// (кто раньше в векторе -- у того уже "будущее" состояние). Отдельно от
// Creature, а не поле в нём: Creature остаётся тем же "мир не лезет внутрь,
// сам ничего не ищет" объектом, что и раньше -- lastOutputs это забота мира.
struct Inhabitant {
    Creature     creature;
    BrainOutputs lastOutputs{};
};

// Точка еды. Не "исчезает" при истощении -- просто amount падает к 0 и
// медленно восстанавливается на месте (см. World::tick()).
struct FoodSource {
    glm::vec3 position{};
    float     amount = 0.0f;
    BerryKind kind   = BerryKind::Raspberry;  // вид ягод: сытность, скорость отрастания, цвет (sim/Berries)
    // Откуда куст: процедурная ячейка (cellX, cellZ) и номер кандидата в ней (local >= 0), либо посаженный/старый куст
    // (local < 0; ячейка -- по положению). По этому мир помнит состояние кустов, пока ячейка "спит" (см. World::streamBushes).
    std::int32_t cellX = 0, cellZ = 0;
    std::int16_t local = -1;
};

// Запись о "спящей" ячейке: чем она отличается от нетронутой процедурной (съеденные кусты, стёртые, посаженные).
struct CellRecordData {
    std::int64_t key = 0;
    double       lastTime = 0.0;                                  // мировое время, когда ячейка уснула (на побудке куст отрастёт за разницу)
    std::vector<std::pair<std::int16_t, float>> deltas;           // номер кандидата -> amount; amount < 0 -- куст стёрт
    std::vector<FoodSource>                     planted;          // посаженные игроком
};

// Population + еда + пространственный индекс для соседских запросов.
// Единственный слой, которому разрешено вызывать world-driven мутаторы
// Creature (feed/rest/mate/recordChild) и Creature::reproduce() -- сам
// Creature этого не делает (см. Creature.hpp). Ноль зависимостей от
// renderer/Vulkan -- Application только читает population() для отрисовки.
// Постройка: пока только точка с поворотом и зерном вида (сам меш рисует Application по зерну).
// Существа её не замечают -- это декорация и мишень для валунов/ластика; обитаемые дома --
// будущая система цивилизаций.
struct Building {
    float         x = 0, z = 0, yaw = 0;  // yaw в градусах, фасад смотрит в +Z до поворота
    std::uint32_t seed = 0;               // вид дома (houseVariant)
};

// Динамическая опасность, которую существа чувствуют и от которой бегут: катящийся валун, место удара.
// Снаружи (Application) выставляется каждый кадр; в сохранение не входит -- это не состояние мира.
struct Hazard {
    glm::vec3 pos{};
    float     radius = 3.0f;  // само тело/зона поражения; страх начинается раньше (kHazardSense)
};

// Счётчики жизни мира -- для отладочного окна и тестов баланса.
struct WorldStats {
    std::uint64_t births = 0, matings = 0;
    std::uint64_t deathsOld = 0, deathsStarved = 0, deathsExhausted = 0, deathsPoison = 0, deathsOther = 0;
    std::uint64_t sabotages = 0;  // сколько раз кто-то подсунул другому ягоды из мести
    std::uint64_t deathsPredation = 0;  // существа, растерзанные хищниками
    std::uint64_t treesFelled = 0, spearsMade = 0, shieldsMade = 0, spearHits = 0, shieldBlocks = 0, skillsLearned = 0;
    std::uint64_t planksLaid = 0, barricadesBuilt = 0, trapsSet = 0;
    std::uint64_t meatKills = 0, bucketsMade = 0, bushesWatered = 0, stockpilesBuilt = 0, hutsBuilt = 0, raids = 0, logsStolen = 0, strikes = 0, deathsConflict = 0, mixedBirths = 0, squadsFormed = 0, squadsDisbanded = 0, squadRaids = 0, huntParties = 0, settlements = 0, successions = 0, sharedMeals = 0, leaderChanges = 0, giftLogs = 0, tradeLogs = 0, alliancesFormed = 0, alliancesBroken = 0, firesBuilt = 0, firesStoked = 0, bitesBlockedByWalls = 0, coldTicks = 0, logsStored = 0, logsTaken = 0, deathsThirst = 0;
};

// События мира для звука и эффектов (не состояние: очищаются takeEvents(), в сохранения не входят).
enum class EventType : std::uint8_t { TreeFelled, Built, Stab, Bitten, Plank, Raid, Strike, Gift };
struct WorldEvent {
    EventType type = EventType::Built;
    glm::vec3 pos{};
};

// Отряд племени: вождь собирает вооружённых бойцов и ведёт их на склад враждебного племени (сбор -> поход -> возвращение).
struct SquadData {
    bool                       active = false;
    std::uint8_t               kind = 0;                  // 0 набег на склад врага, 1 охотничья партия, 2 поселенческая экспедиция
    std::uint8_t               target = 0, phase = 0;   // племя-цель (набег); 0 сбор, 1 поход, 2 возвращение
    std::uint32_t              baseline = 0;             // охота: счёт meatKills к началу похода
    float                      timer = 0.0f, cooldown = 0.0f;
    glm::vec2                  goal{};                    // куда идти в текущей фазе (поход/возвращение)
    std::vector<std::uint64_t> members;
};

// Полное состояние мира для сохранения/загрузки (всё, что нужно, чтобы продолжить симуляцию
// бит в бит: в том числе состояние ГСЧ и ожидающие пинги горя).
struct WorldState {
    std::string                                          rng;      // текст состояния std::mt19937
    std::uint64_t                                        nextId = 1;
    std::vector<Inhabitant>                              population;
    std::vector<FoodSource>                              food;
    std::vector<Building>                                buildings;
    std::vector<std::pair<std::uint64_t, float>>         pendingGrief;  // по возрастанию id
    Fauna::State                                         fauna;         // дикие животные
    std::vector<Structure>                               structures;    // баррикады и ловушки
    std::array<float, 16>                                relations{};   // отношения племён [a*4+b], -1 вражда .. 1 дружба
    std::array<std::uint64_t, 4>                         leaders{};
    std::array<SquadData, 4>                             squads{};      // отряды племён (индекс -- племя)     // вожди племён (id существа, 0 -- нет)
    // Потоковая подгрузка кустов: мировое время, материализованные ячейки и записи о спящих (старые сохранения -- пусто).
    float                                                streamTimer = 100.0f;   // таймер подгрузки кустов (для продолжения бит в бит)
    bool                                                 streaming = false;
    double                                               time = 0.0;
    std::vector<std::int64_t>                            loadedCells;
    std::vector<CellRecordData>                          cellRecords;
};

class World {
public:
    static constexpr float       kWorldHalfSize     = 100.0f;
    static constexpr std::size_t kInitialPopulation  = 24;
    static constexpr std::size_t kMaxPopulation       = 200;  // защитный потолок: голод пока не убивает (см. CLAUDE.md), иначе популяция не ограничена ничем
    static constexpr std::size_t kFoodCount           = 30;

    explicit World(std::uint32_t seed);

    [[nodiscard]] WorldState exportState() const;
    // false -- состояние некорректно (ГСЧ не разобрался); мир при этом не тронут.
    bool importState(const WorldState& state);

    // Один шаг: сенсы -> мозг -> резолв действий -> размножение -> смерть -> еда.
    // terrain != nullptr -- существа подчиняются рельефу (уклон, вода); nullptr --
    // плоский мир без ограничений (тесты, декоративная колония в меню).
    // Рубка деревьев меняет рельеф (Terrain::fellTree) -- поэтому террейн неконстантный; области для пересборки чанков -- в takeDirty().
    void tick(float dt, Terrain* terrain = nullptr);
    [[nodiscard]] std::vector<Terrain::Rect> takeDirty() { return std::exchange(m_dirty, {}); }
    [[nodiscard]] std::vector<WorldEvent>    takeEvents() { return std::exchange(m_events, {}); }

    // Опасности на текущий кадр (заменяют предыдущие). Существа в радиусе kHazardSense от края чувствуют угрозу.
    void setHazards(std::span<const Hazard> hazards) { m_hazards.assign(hazards.begin(), hazards.end()); }
    [[nodiscard]] const WorldStats& stats() const noexcept { return m_stats; }

    // Переселяет существ и еду, оказавшихся в воде, на случайную сушу в границах мира.
    void settleOnLand(const Terrain& terrain);
    // Включает процедурные ягоды: заменяет прежнюю еду, материализует кусты стартовой области (при создании мира и в
    // "Новом мире"). Дальше мир сам подгружает ячейки там, где есть существа и точки интереса (setFocus), и усыпляет
    // остальные, запоминая съеденное/посаженное/стёртое (streamBushes).
    void growBerryBushes(const Terrain& terrain);
    // Точки интереса помимо существ (камера): вокруг них тоже "просыпаются" кусты. Задаются снаружи каждый кадр.
    void setFocus(std::span<const glm::vec2> points) { m_focus.assign(points.begin(), points.end()); }
    // Материализует ячейки вокруг (x, z) (нужно перед посадкой куста в ещё не загруженном месте).
    void ensureBushesAt(const Terrain& terrain, float x, float z);
    // Есть ли поблизости процедурный или посаженный куст: для запретной зоны деревьев (кусты спящих ячеек тоже блокируют).
    [[nodiscard]] bool bushNear(const Terrain& terrain, float x, float z, float radius) const;
    [[nodiscard]] std::size_t loadedBushCells() const noexcept { return m_loadedCells.size(); }
    [[nodiscard]] double      time() const noexcept { return m_time; }
    // Смена дня и ночи по мировому времени: высота солнца -1..1 (начало мира -- утро), "ночность" 0..1 (плавно, ~40% суток тёмные).
    // Существа спят ночью только в мире с рельефом (тесты без рельефа живут по-старому).
    static constexpr float kDayLength = 300.0f;
    [[nodiscard]] static float sunElevation(double time) noexcept;
    [[nodiscard]] static float nightness(double time) noexcept;
    [[nodiscard]] float        sunElevation() const noexcept { return sunElevation(m_time); }
    [[nodiscard]] float        nightness() const noexcept { return nightness(m_time); }
    // Времена года и погода: чистые функции мирового времени (ничего не хранится и не сохраняется). Год = 4 суток; мир начинается весной.
    // coldness 0..1 (зимний холод), foodFactor -- множитель отрастания ягод (лето богаче, зима скуднее), rain 0..1 (первые ~200 с сухо).
    static constexpr float kYearLength = 4.0f * kDayLength;
    [[nodiscard]] static float yearPhase(double time) noexcept;      // 0..1, 0 -- начало весны
    [[nodiscard]] static int   season(double time) noexcept;         // 0 весна, 1 лето, 2 осень, 3 зима
    [[nodiscard]] static const char* seasonName(int season) noexcept;
    [[nodiscard]] static float coldness(double time) noexcept;
    [[nodiscard]] static float foodFactor(double time) noexcept;
    [[nodiscard]] static float rain(double time) noexcept;
    [[nodiscard]] float        coldness() const noexcept { return coldness(m_time); }
    [[nodiscard]] float        rain() const noexcept { return rain(m_time); }

    [[nodiscard]] std::span<const Inhabitant>  population() const noexcept { return m_population; }
    [[nodiscard]] std::span<const FoodSource>  food() const noexcept { return m_food; }
    [[nodiscard]] std::size_t populationCount() const noexcept { return m_population.size(); }
    // Самое позднее поколение среди живых (0 -- все созданы миром/богом, ребёнок = max(родителей)+1).
    [[nodiscard]] int maxGeneration() const noexcept { int g = 0; for (const Inhabitant& in : m_population) g = std::max<int>(g, in.creature.generation); return g; }
    [[nodiscard]] std::size_t foodCount() const noexcept { return m_food.size(); }

    // ── Руки бога: вмешательство игрока в мир (см. sim/Powers) ──────────────
    // Новое существо в точке (x, z). Черты из traits (индекс -> [-1,1]) заменяют
    // случайные; мозг всегда случайный. false -- популяция уже на потолке.
    bool spawnCreature(float x, float z, std::span<const std::pair<Genome::TraitIndex, float>> traits = {});
    // Убирает всех существ в круге; возвращает сколько убрано.
    std::size_t removeCreatures(float x, float z, float radius);
    [[nodiscard]] std::size_t countCreatures(float x, float z, float radius) const;

    static constexpr std::size_t kMaxBuildings   = 300;
    static constexpr float       kBuildingSpacing = 6.0f;  // ближе центры не ставим
    // false -- слишком близко к другому дому или их уже kMaxBuildings.
    bool addBuilding(float x, float z, float yaw, std::uint32_t seed);
    std::size_t removeBuildings(float x, float z, float radius);
    [[nodiscard]] std::span<const Building> buildings() const noexcept { return m_buildings; }

    // ── Постройки существ: баррикады и ловушки (мосты -- в Terrain). Ставит существо через tick; бог/тесты -- напрямую.
    static constexpr std::size_t kMaxStructures = 240;
    // false -- нельзя: вода, слишком близко к другой постройке, потолок, баррикада/ловушка скучены (3+ в 8 единицах).
    bool placeStructure(const Terrain* terrain, StructureKind kind, float x, float z, float yaw);
    [[nodiscard]] std::span<const Structure> structures() const noexcept { return m_structures; }

    // ── Племена ──
    static constexpr int kMaxTribes = 4;   // индексы 1..3 -- племена, 0 -- ничьи
    // Делит существ на `count` лагерей (по кругу), расселяет их кучками на суше порознь, у каждого лагеря ставит склад с запасом
    // (хозяин -- племя) и задаёт случайные отношения между племенами (от вражды до дружбы). Вызывается при создании мира.
    void formTribes(const Terrain& terrain, int count = 3);
    [[nodiscard]] float relation(int a, int b) const noexcept { return (a > 0 && b > 0 && a < kMaxTribes && b < kMaxTribes) ? m_relations[a][b] : 0.0f; }
    void setRelation(int a, int b, float v) noexcept;
    [[nodiscard]] std::size_t tribeCount(int tribe) const noexcept;
    // Вождь племени (id; 0 -- нет): раз в kElectionSeconds выбирается лучший по зрелости, храбрости, общительности, детям, уважению и здоровью
    // (действующему -- бонус, чтобы не менялись по пустякам). Соплеменники держатся поближе к вождю.
    static constexpr double kElectionSeconds = 5.0;
    [[nodiscard]] const SquadData& squad(int tribe) const noexcept { static const SquadData none; return (tribe > 0 && tribe < kMaxTribes) ? m_squads[tribe] : none; }
    [[nodiscard]] std::uint64_t leaderId(int tribe) const noexcept { return (tribe > 0 && tribe < kMaxTribes) ? m_leaders[tribe] : 0; }
    // Союз двух племён (отношения выше 0.6, расходятся ниже 0.3 -- с гистерезисом); только для статистики и показа.
    [[nodiscard]] bool allied(int a, int b) const noexcept { return a > 0 && b > 0 && a < kMaxTribes && b < kMaxTribes && m_allied[a][b]; }
    // Можно ли племени tribe пользоваться чужим складом: ничей, свой или дружественный (отношения > 0.3).
    [[nodiscard]] bool pileOpenTo(const Structure& st, int tribe) const noexcept {
        return st.owner == 0 || st.owner == static_cast<std::uint64_t>(tribe) || (tribe > 0 && st.owner < static_cast<std::uint64_t>(kMaxTribes) && m_relations[tribe][st.owner] > 0.3f);
    }

    // ── Дикие животные (sim/Fauna): травоядные и хищники по биомам ──
    // Заселяет игровую область заново (при создании мира и в "Новом мире"); ГСЧ мировой.
    void growWildlife(const Terrain& terrain);
    [[nodiscard]] const Fauna& fauna() const noexcept { return m_fauna; }
    bool spawnAnimal(Species s, float x, float z) { return m_fauna.spawn(s, x, z, m_rng); }
    bool spawnNaturalAnimal(const Terrain& terrain, float x, float z) { return m_fauna.spawnNatural(terrain, x, z, m_rng); }
    std::size_t removeAnimals(float x, float z, float radius) { return m_fauna.removeInCircle(x, z, radius); }

    static constexpr std::size_t kMaxFood      = 8000;   // потолок материализованных кустов
    static constexpr float       kBushSpacing  = 1.6f;   // ближе центры не сажаем
    // Куст в (x, z) данного вида, полный. false -- слишком близко к другому кусту или их уже kMaxFood.
    bool plantBush(float x, float z, BerryKind kind);
    // То же, но вид -- по биому точки (случайный из подходящих); false как у plantBush.
    bool plantNaturalBush(const Terrain& terrain, float x, float z);
    std::size_t removeBushes(float x, float z, float radius);
    // Ближайший куст не дальше radius (индекс в food()); false -- нет.
    [[nodiscard]] bool nearestBush(float x, float z, float radius, std::size_t& index) const;

private:
    Rng           m_rng;
    std::uint64_t m_nextId = 1;

    std::vector<Inhabitant>  m_population;
    std::vector<FoodSource>  m_food;
    std::vector<Building>    m_buildings;
    std::vector<Hazard>      m_hazards;
    WorldStats               m_stats;
    Fauna                    m_fauna;
    std::vector<Terrain::Rect> m_dirty;
    std::vector<WorldEvent>    m_events;
    void pushEvent(EventType t, glm::vec3 pos) { if (m_events.size() < 64) m_events.push_back({t, pos}); }
    std::vector<Structure>     m_structures;
    float                      m_relations[kMaxTribes][kMaxTribes]{};
    bool                       m_allied[kMaxTribes][kMaxTribes]{};
    std::uint64_t              m_leaders[kMaxTribes]{};
    std::int64_t               m_electionSlot = -1;
    void electLeaders();
    SquadData m_squads[kMaxTribes];
    void updateSquads(float dt, const Terrain& terrain, const std::unordered_map<std::uint64_t, std::size_t>& idIndex);
    void disbandSquad(int tribe, float cooldown);

    // Потоковая подгрузка кустов (см. growBerryBushes/streamBushes в World.cpp).
    bool                          m_streaming = false;
    double                        m_time = 0.0;
    float                         m_streamTimer = 100.0f;
    std::vector<glm::vec2>        m_focus;
    std::unordered_set<std::int64_t>                     m_loadedCells;
    std::unordered_map<std::int64_t, CellRecordData>     m_cellRecords;
    mutable std::unordered_map<std::int64_t, std::vector<BushSpec>> m_specCache;   // кэш процедурных ячеек для bushNear
    void loadCell(const Terrain& terrain, std::int32_t cx, std::int32_t cz);
    void unloadCells(const Terrain& terrain, const std::vector<std::int64_t>& keys);
    void streamBushes(const Terrain& terrain, float dt);

    // id выжившего родственника только что умершего существа -> ping на
    // ровно следующий тик (см. SenseInputs::lovedOneDied -- тот же
    // одноразовый контракт). Заполняется в конце tick(), где умер кто-то;
    // читается и полностью очищается в начале следующего tick().
    std::unordered_map<std::uint64_t, float> m_pendingGrief;
};

}  // namespace wsim
