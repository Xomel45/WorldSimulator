#pragma once

#include "sim/Biomes.hpp"
#include "sim/Genome.hpp"  // Rng
#include "sim/Structures.hpp"
#include "sim/Terrain.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wsim {

// Дикие животные: отдельная от "существ" (главных героев с геномом и мозгом) подсистема. Простые виды с врождённым
// поведением: травоядные пасутся, держатся стадом и убегают, хищники охотятся на травоядных (а голодные -- и на существ).
// Виды привязаны к биомам (как ягоды и деревья): в тайге волки и олени, в саванне львы и антилопы, в джунглях ягуары и тапиры...
enum class Species : std::uint8_t { Hare, Deer, Antelope, Tapir, Wolf, Fox, Lion, Jaguar };
inline constexpr int kSpeciesCount = 8;

enum class Diet : std::uint8_t { Herbivore, Predator };

struct SpeciesDef {
    const char* id;          // для модов: "wolf"...
    const char* name;        // Русское название
    Diet        diet;
    float       size;        // масштаб модели и "вес": длина тела ~ size единиц
    float       speed;       // максимальная скорость, ед./с
    float       sense;       // дальность зрения: угрозы (травоядные), добыча (хищники)
    float       panic;       // травоядное убегает от хищника, подошедшего ближе (и от существа ближе panic*0.3)
    int         groupMin, groupMax;                    // размер группы при расселении
    std::array<float, kBiomeCount> biome;               // вес присутствия по биомам (Biome по порядку); 0 -- не живёт
    int         cap;         // потолок популяции вида (размножение останавливается)
    float       maturity;    // возраст взрослости, с
    float       breedCooldown;  // перерыв между потомством, с
    float       lifespan;    // с
    std::uint32_t prey;      // битовая маска видов, на которых охотится (хищники)
    bool        huntsCreatures;  // охотится ли на существ, когда добычи нет и голод сильный
    bool        swims;       // может заходить в воду
};

[[nodiscard]] const SpeciesDef& speciesDef(Species s) noexcept;
[[nodiscard]] bool speciesFromId(std::string_view id, Species& out) noexcept;
[[nodiscard]] inline bool isPredator(Species s) noexcept { return speciesDef(s).diet == Diet::Predator; }
// Подходит ли вид биому (вес > 0).
// Ночные звери (лиса, ягуар): днём дремлют, охотятся в сумерках и ночью. Перелётные травоядные (заяц, олень, антилопа): зимой тянутся
// к тёплым местам, весной возвращаются в свои биомы.
[[nodiscard]] inline bool isNocturnal(Species s) noexcept { return s == Species::Fox || s == Species::Jaguar; }
[[nodiscard]] inline bool isMigratory(Species s) noexcept { return s == Species::Deer || s == Species::Antelope || s == Species::Hare; }
[[nodiscard]] inline bool speciesLivesIn(Species s, Biome b) noexcept { return speciesDef(s).biome[static_cast<std::size_t>(b)] > 0.0f; }
// Какой вид селится в биоме: u в [0,1) выбирает по весам (травоядные и хищники вместе); false -- в биоме никого нет.
[[nodiscard]] bool pickSpeciesFor(Biome biome, float u, Species& out) noexcept;

enum class AnimalState : std::uint8_t { Wander, Rest, Graze, Flee, Hunt, Feed, Count };
[[nodiscard]] const char* animalStateName(AnimalState s) noexcept;  // по-русски

struct Animal {
    std::uint64_t id = 0;
    Species       species = Species::Hare;
    glm::vec3     position{};
    float         heading = 0.0f;       // радианы: направление в плоскости XZ (cos -> x, sin -> z)
    float         speedNow = 0.0f;      // текущая скорость, ед./с
    float         hunger = 0.3f;        // 0 сыт .. 1 при смерти от голода
    float         health = 1.0f;
    float         age = 0.0f;
    float         stamina = 1.0f;       // бег на пределе: тратится, восстанавливается отдыхом
    float         breedCooldown = 0.0f;
    float         attackCooldown = 0.0f;
    float         searchCooldown = 0.0f;   // хищник: пауза между поисками добычи (после неудачной погони)
    float         stateTimer = 0.0f;
    float         wanderTimer = 0.0f;
    AnimalState   state = AnimalState::Wander;
    std::uint64_t targetId = 0;         // кого преследует (животное или существо)
    bool          targetIsCreature = false;
    float         sinceBite = 100.0f;   // сколько прошло с последнего укуса существа (чтобы засчитать добычу)
    float         stun = 0.0f;          // > 0 -- попал в ловушку, стоит (не сохраняется: секунды)
    bool          alive = true;
    std::uint8_t  deathCause = 0;       // 1 старость, 2 голод, 3 съеден, 4 убит богом, 5 заколот копьём
};

// Сводка мира для животных: существа (позиции для страха/охоты) и опасности наружу.
struct CreatureView {
    std::uint64_t id = 0;
    glm::vec3     position{};
};
struct Bite {
    std::uint64_t creatureId = 0;
    float         damage = 0.0f;
    std::uint64_t attackerId = 0;   // кто укусил (копьё отвечает ударом)
};

struct FaunaStats {
    std::uint64_t births = 0, kills = 0, creatureBites = 0, trapsTriggered = 0, deathsOld = 0, deathsStarved = 0, immigrants = 0;
};

// Мир диких животных. Не знает о World: получает позиции существ списком и возвращает укусы (World применяет их к существам).
// Всё детерминировано относительно переданного Rng (мировой ГСЧ -- результат воспроизводим и после загрузки сохранения).
class Fauna {
public:
    static constexpr std::size_t kMaxAnimals = 160;
    static constexpr float       kMinSpacing = 1.0f;

    [[nodiscard]] std::span<const Animal> animals() const noexcept { return m_animals; }
    [[nodiscard]] std::size_t count() const noexcept { return m_animals.size(); }
    [[nodiscard]] std::size_t count(Species s) const noexcept;
    [[nodiscard]] const FaunaStats& stats() const noexcept { return m_stats; }

    // Заселение стартовой области [-halfSize, halfSize]^2 по биомам: группы травоядных и редкие хищники (если для них есть добыча).
    // Её ячейки считаются исследованными; остальной мир заселяется по мере появления там существ и камеры (см. tick, halfSize <= 0).
    void populate(const Terrain& terrain, Rng& rng, float halfSize);
    // Животное данного вида в (x, z) (взрослое; при adult=false -- детёныш). false -- потолок или вид не уживается рядом с другим животным.
    bool spawn(Species s, float x, float z, Rng& rng, bool adult = true);
    // То же, но вид -- по биому точки. false -- в биоме нет видов или spawn не удался.
    bool spawnNatural(const Terrain& terrain, float x, float z, Rng& rng);
    std::size_t removeInCircle(float x, float z, float radius);
    // Ближайшее живое животное не дальше radius (индекс); false -- нет.
    [[nodiscard]] bool nearest(float x, float z, float radius, std::size_t& index) const;

    // Один шаг. terrain может быть nullptr (плоский мир без воды -- тесты). creatures -- позиции существ (для страха и охоты).
    // Укусы по существам дописываются в bites. halfSize > 0 -- игровая область квадрат [-halfSize, halfSize] (мягкие границы, миграция
    // с краёв); halfSize <= 0 -- мир без границ: вокруг существ и точек интереса focus впервые попавшие в поле ячейки заселяются
    // (раз и навсегда -- ячейка запоминается как исследованная), а вымирающие виды пополняются кольцом вокруг этих точек.
    void tick(float dt, const Terrain* terrain, std::span<const CreatureView> creatures, Rng& rng, float halfSize, std::vector<Bite>& bites,
              std::span<const glm::vec2> focus = {}, std::vector<Structure>* structures = nullptr, float night = 0.0f, float cold = 0.0f);   // night 0..1: хищники ночью голоднее и зорче

    // Удар по животному (копьё существа): здоровье падает, раненый убегает; убитое снимается со следующим шагом (deathCause 5).
    bool hurt(std::uint64_t id, float damage);   // true -- животное погибло от удара

    // Сохранение.
    struct State {
        std::uint64_t       nextId = 1;
        std::vector<Animal> animals;
        float               immigrationTimer = 0.0f;
        std::vector<std::int64_t> explored;   // заселённые ячейки мира без границ (по возрастанию)
        float               streamTimer = 100.0f;   // таймер проверки ячеек (сохраняется: иначе продолжение после загрузки не совпало бы бит в бит)
    };
    [[nodiscard]] State exportState() const;
    bool importState(const State& s);

private:
    std::vector<Animal> m_animals;
    std::uint64_t       m_nextId = 1;
    float               m_immigrationTimer = 0.0f;
    float               m_streamTimer = 100.0f;
    std::vector<std::int64_t> m_explored;     // отсортирован
    FaunaStats          m_stats;

    static constexpr float kWildCell = 48.0f;   // ячейка заселения мира без границ
    void spawnGroupAt(Species s, glm::vec2 centre, const Terrain& terrain, Rng& rng, float halfSize);
    void streamWildlife(float dt, const Terrain& terrain, std::span<const CreatureView> creatures, std::span<const glm::vec2> focus, Rng& rng);
    bool markExplored(std::int32_t cx, std::int32_t cz);   // true -- ячейка была новой
};

}  // namespace wsim
