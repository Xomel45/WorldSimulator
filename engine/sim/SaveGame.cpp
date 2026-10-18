#include "sim/SaveGame.hpp"

#include "sim/ByteIO.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace wsim {

namespace {

constexpr char kMagic[8] = {'W', 'S', 'I', 'M', 'S', 'A', 'V', 'E'};

// Потолки на размеры из файла: защита от битых и подделанных данных (иначе счётчик на 4 миллиарда
// заставил бы выделить гигабайты). Подобраны с большим запасом над реальной игрой.
constexpr std::size_t kMaxCreatures = 100000, kMaxFood = 100000, kMaxBuildings = 100000, kMaxBodies = 10000;
constexpr std::size_t kMaxChildren = 100000, kMaxTiles = 200000, kMaxGrief = 1000000;

// Секции полезной нагрузки: тег (4 байта) + длина + данные; незнакомые пропускаем (совместимость вперёд).
constexpr std::uint32_t tag(const char (&s)[5]) {
    return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 | static_cast<std::uint32_t>(s[2]) << 16 |
           static_cast<std::uint32_t>(s[3]) << 24;
}
constexpr std::uint32_t kTagTerrain = tag("TERR"), kTagWorld = tag("WRLD"), kTagPhysics = tag("PHYS"), kTagCamera = tag("CAMR"),
                         kTagStream = tag("STRM"), kTagStructs = tag("STRC"), kTagStructs2 = tag("STR2"),   // потоковые кусты: время, загруженные ячейки, записи о спящих, происхождение кустов
                         kTagFauna = tag("FAUN"),    // дикие животные (старые сохранения без неё -- мир без зверей)
                         kTagBerries = tag("BRRY");  // вид ягод каждого куста (по порядку WRLD.food); старые сохранения -- без неё, все кусты малина

void writeMeta(ByteWriter& w, const SaveMeta& m) {
    w.str(m.name);
    w.i64(m.unixTime);
    w.u32(m.seed);
    w.f32(m.playSeconds);
    w.u32(m.population);
    w.u32(m.buildings);
    w.u32(m.boulders);
    w.u32(m.editedTiles);
}

SaveMeta readMeta(ByteReader& r) {
    SaveMeta m;
    m.name        = r.str(256);
    m.unixTime    = r.i64();
    m.seed        = r.u32();
    m.playSeconds = r.f32();
    m.population  = r.u32();
    m.buildings   = r.u32();
    m.boulders    = r.u32();
    m.editedTiles = r.u32();
    return m;
}

void writeCreature(ByteWriter& w, const Inhabitant& in, std::uint32_t version) {
    const Creature& c = in.creature;
    w.floats(c.genome.genes());
    const Needs& n = c.needs;
    for (float v : {n.hunger, n.energy, n.fear, n.libido, n.social, n.respect, n.resentment, n.grief}) w.f32(v);
    w.vec3(c.position);
    w.vec3(c.velocity);
    w.vec3(c.homePosition);
    w.f32(c.age);
    w.boolean(c.alive);
    w.u64(c.id);
    w.u64(c.bondedPartnerId);
    w.u64(c.parentIds[0]);
    w.u64(c.parentIds[1]);
    w.u32(static_cast<std::uint32_t>(c.childIds.size()));
    for (auto id : c.childIds) w.u64(id);
    w.floats(c.learnedWeights);
    const BrainOutputs& o = in.lastOutputs;
    for (float v : {o.moveX, o.moveZ, o.eatDesire, o.fleeDesire, o.mateDesire, o.restDesire, o.defendDesire, o.alarmCall, o.matingCall, o.dominanceDisplay})
        w.f32(v);
    if (version < 2) return;
    // v2: здоровье, размножение, поведение.
    w.f32(c.health);
    w.f32(c.matingCooldown);
    w.u8(static_cast<std::uint8_t>(c.behavior));
    w.f32(c.behaviorTimer);
    w.f32(c.wanderHeading);
    w.vec3(c.foodMemory);
    w.f32(c.foodMemoryAge);
    if (version < 3) return;
    // v3: яд; вкус ягод (число записей пишем явно). В v3 вкус хранился по ВИДАМ растений -- от него отказались
    // (теперь по внешнему виду, v4), так что при записи v3 (тесты совместимости) записей нет.
    w.f32(c.toxin);
    if (version == 3) {
        w.u8(0);
    } else {
        w.u8(static_cast<std::uint8_t>(kBerryLookCount));
        for (int k = 0; k < kBerryLookCount; ++k) {
            w.f32(c.taste[static_cast<std::size_t>(k)]);
            w.u8(c.tasteSamples[static_cast<std::size_t>(k)]);
        }
    }
    if (version < 4) return;
    // v4: понос, тошнота, галлюцинации, рвотный приступ.
    for (float v : {c.diarrhea, c.nausea, c.hallucination, c.vomitStun, c.visionTimer, c.visionHeading}) w.f32(v);
    if (version < 5) return;
    // v5: последняя трапеза, зуб на соседа, ягоды в руках.
    w.u8(c.mealLook);
    w.f32(c.sinceBite);
    w.u64(c.grudgeId);
    w.f32(c.grudge);
    w.u8(c.carryKind);
    w.f32(c.carryAmount);
    w.f32(c.carryAge);
    if (version < 6) return;
    w.f32(c.exploreTimer);  // v6: уход от кормовой точки
    if (version < 7) return;
    // v7: ресурсы и орудия, кэш дерева для рубки.
    w.u8(c.skills);
    for (float v : {c.wood, c.spearDur, c.shieldDur, c.dangerMemory, c.chopProgress, c.craftProgress, c.treeX, c.treeZ, c.treeSearch}) w.f32(v);
    w.boolean(c.treeKnown);
    w.u32(static_cast<std::uint32_t>(c.treeCx));
    w.u32(static_cast<std::uint32_t>(c.treeCz));
    if (version < 8) return;
    w.f32(c.buildProgress);   // v8: стройка (запрос исполняется в тот же тик и не хранится)
    w.f32(c.buildTimer);
    if (version < 9) return;
    for (float v : {c.thirst, c.water, c.huntTimer, c.waterX, c.waterZ, c.waterSearch}) w.f32(v);   // v9
    w.boolean(c.hasBucket);
    w.boolean(c.waterKnown);
}

bool readCreature(ByteReader& r, Inhabitant& in, std::uint32_t version) {
    Creature& c = in.creature;
    const std::vector<float> genes = r.floats(Genome::kGeneCount);
    if (!r.ok() || !Genome::fromGenes(genes, c.genome)) return false;
    Needs& n = c.needs;
    for (float* v : {&n.hunger, &n.energy, &n.fear, &n.libido, &n.social, &n.respect, &n.resentment, &n.grief}) *v = r.f32();
    c.position        = r.vec3();
    c.velocity        = r.vec3();
    c.homePosition    = r.vec3();
    c.age             = r.f32();
    c.alive           = r.boolean();
    c.id              = r.u64();
    c.bondedPartnerId = r.u64();
    c.parentIds[0]    = r.u64();
    c.parentIds[1]    = r.u64();
    const std::uint32_t children = r.count(kMaxChildren, 8);
    c.childIds.reserve(children);
    for (std::uint32_t i = 0; i < children; ++i) c.childIds.push_back(r.u64());
    c.learnedWeights = r.floats(Genome::kPlasticBrainWeightCount);  // пусто (ещё не тикало) или ровно столько, сколько пластичных весов
    if (!c.learnedWeights.empty() && c.learnedWeights.size() != Genome::kPlasticBrainWeightCount) return false;
    BrainOutputs& o = in.lastOutputs;
    for (float* v : {&o.moveX, &o.moveZ, &o.eatDesire, &o.fleeDesire, &o.mateDesire, &o.restDesire, &o.defendDesire, &o.alarmCall, &o.matingCall, &o.dominanceDisplay})
        *v = r.f32();
    if (version >= 2) {
        c.health         = std::clamp(r.f32(), 0.0f, 1.0f);
        c.matingCooldown = std::max(0.0f, r.f32());
        const std::uint8_t b = r.u8();
        c.behavior       = b < static_cast<std::uint8_t>(Behavior::Count) ? static_cast<Behavior>(b) : Behavior::Wander;
        c.behaviorTimer  = r.f32();
        c.wanderHeading  = r.f32();
        c.foodMemory     = r.vec3();
        c.foodMemoryAge  = r.f32();
    }
    if (version >= 3) {
        c.toxin = std::clamp(r.f32(), 0.0f, 2.0f);
        const std::uint8_t entries = r.u8();
        for (std::uint8_t k = 0; k < entries && r.ok(); ++k) {
            const float taste = r.f32();
            const std::uint8_t samples = r.u8();
            // v3 хранил вкус по видам растений (теперь по внешнему виду): записи читаем, но не применяем.
            if (version >= 4 && k < kBerryLookCount) { c.taste[k] = std::isfinite(taste) ? std::clamp(taste, -3.0f, 3.0f) : 0.0f; c.tasteSamples[k] = samples; }
        }
    }
    if (version >= 4) {
        auto level = [&](float hi) { const float v = r.f32(); return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 0.0f; };
        c.diarrhea      = level(2.0f);
        c.nausea        = level(2.0f);
        c.hallucination = level(2.0f);
        c.vomitStun     = level(10.0f);
        c.visionTimer   = level(10.0f);
        c.visionHeading = r.f32();
    }
    if (version >= 5) {
        auto level = [&](float hi) { const float v = r.f32(); return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 0.0f; };
        const std::uint8_t meal = r.u8();
        c.mealLook  = meal < kBerryLookCount ? meal : 255;
        c.sinceBite = level(1000.0f);
        c.grudgeId  = r.u64();
        c.grudge    = level(1.0f);
        const std::uint8_t carry = r.u8();
        c.carryAmount = level(1.0f);
        c.carryAge    = level(100.0f);
        c.carryKind   = carry < kBerryKindCount ? carry : 255;
        if (c.carryKind == 255) c.carryAmount = 0.0f;
    }
    if (version >= 6) {
        const float e = r.f32();
        c.exploreTimer = std::isfinite(e) ? std::clamp(e, 0.0f, 60.0f) : 0.0f;
    }
    if (version >= 7) {
        auto level = [&](float hi) { const float v = r.f32(); return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 0.0f; };
        c.skills       = static_cast<std::uint8_t>(r.u8() & kAllSkills);
        c.wood         = level(kMaxWood);
        c.spearDur     = level(1.0f);
        c.shieldDur    = level(1.0f);
        c.dangerMemory = level(300.0f);
        c.chopProgress = level(1.0f);
        c.craftProgress = level(1.0f);
        const float tx = r.f32(), tz = r.f32();
        c.treeX = std::isfinite(tx) ? tx : 0.0f;
        c.treeZ = std::isfinite(tz) ? tz : 0.0f;
        c.treeSearch = level(10.0f);
        c.treeKnown = r.boolean();
        c.treeCx = static_cast<std::int32_t>(r.u32());
        c.treeCz = static_cast<std::int32_t>(r.u32());
    }
    if (version >= 8) {
        auto level = [&](float hi) { const float v = r.f32(); return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 0.0f; };
        c.buildProgress = level(1.0f);
        c.buildTimer    = level(60.0f);
    }
    if (version >= 9) {
        auto level = [&](float hi) { const float v = r.f32(); return std::isfinite(v) ? std::clamp(v, 0.0f, hi) : 0.0f; };
        c.thirst = level(1.0f);
        c.water  = level(kBucketCapacity);
        c.huntTimer = level(10.0f);
        const float wx = r.f32(), wz = r.f32();
        c.waterX = std::isfinite(wx) ? wx : 0.0f;
        c.waterZ = std::isfinite(wz) ? wz : 0.0f;
        c.waterSearch = level(10.0f);
        c.hasBucket   = r.boolean();
        c.waterKnown  = r.boolean();
    }
    return r.ok();
}

void section(ByteWriter& out, std::uint32_t t, const ByteWriter& body) {
    out.u32(t);
    out.u32(static_cast<std::uint32_t>(body.bytes().size()));
    out.raw(body.bytes());
}

}  // namespace

SaveGame captureSave(const std::string& name, std::int64_t unixTime, float playSeconds, const Terrain& terrain, const World& world,
                     const PhysicsWorld& physics, const CameraState& camera) {
    SaveGame s;
    s.terrainSeed     = terrain.seed();
    s.terrainHalfSize = terrain.halfSize();
    s.terrainEdits    = terrain.exportEdits();
    s.felledTrees     = terrain.exportFelled();
    s.bridges         = terrain.exportBridges();
    s.world           = world.exportState();
    s.bodies          = physics.bodies();
    s.camera          = camera;
    s.meta.name        = name;
    s.meta.unixTime    = unixTime;
    s.meta.seed        = s.terrainSeed;
    s.meta.playSeconds = playSeconds;
    s.meta.population  = static_cast<std::uint32_t>(s.world.population.size());
    s.meta.buildings   = static_cast<std::uint32_t>(s.world.buildings.size());
    s.meta.boulders    = static_cast<std::uint32_t>(s.bodies.size());
    s.meta.editedTiles = static_cast<std::uint32_t>(s.terrainEdits.size());
    return s;
}

std::vector<std::uint8_t> serializeSave(const SaveGame& g, std::uint32_t version) {
    ByteWriter payload;
    {
        ByteWriter b;
        b.u32(g.terrainSeed);
        b.f32(g.terrainHalfSize);
        b.u32(static_cast<std::uint32_t>(g.terrainEdits.size()));
        for (const auto& t : g.terrainEdits) {
            b.u32(static_cast<std::uint32_t>(t.tx));
            b.u32(static_cast<std::uint32_t>(t.tz));
            b.floats(t.values);
        }
        b.u32(static_cast<std::uint32_t>(g.felledTrees.size()));   // в конце секции: старые файлы без него читаются
        for (std::int64_t k : g.felledTrees) b.i64(k);
        b.u32(static_cast<std::uint32_t>(g.bridges.size()));       // v8
        for (std::int64_t k : g.bridges) b.i64(k);
        section(payload, kTagTerrain, b);
    }
    {
        ByteWriter b;
        b.str(g.world.rng);
        b.u64(g.world.nextId);
        b.u32(static_cast<std::uint32_t>(g.world.population.size()));
        for (const auto& in : g.world.population) writeCreature(b, in, version);
        b.u32(static_cast<std::uint32_t>(g.world.food.size()));
        for (const auto& f : g.world.food) { b.vec3(f.position); b.f32(f.amount); }
        b.u32(static_cast<std::uint32_t>(g.world.buildings.size()));
        for (const auto& h : g.world.buildings) { b.f32(h.x); b.f32(h.z); b.f32(h.yaw); b.u32(h.seed); }
        b.u32(static_cast<std::uint32_t>(g.world.pendingGrief.size()));
        for (const auto& [id, v] : g.world.pendingGrief) { b.u64(id); b.f32(v); }
        section(payload, kTagWorld, b);
    }
    {
        ByteWriter b;
        b.u32(static_cast<std::uint32_t>(g.world.food.size()));
        for (const auto& f : g.world.food) b.u8(static_cast<std::uint8_t>(f.kind));
        section(payload, kTagBerries, b);
    }
    {
        ByteWriter b;
        const Fauna::State& f = g.world.fauna;
        b.u64(f.nextId);
        b.f32(f.immigrationTimer);
        b.u32(static_cast<std::uint32_t>(f.animals.size()));
        for (const Animal& a : f.animals) {
            b.u64(a.id);
            b.u8(static_cast<std::uint8_t>(a.species));
            b.vec3(a.position);
            for (float v : {a.heading, a.speedNow, a.hunger, a.health, a.age, a.stamina, a.breedCooldown, a.attackCooldown, a.searchCooldown, a.stateTimer, a.wanderTimer, a.sinceBite}) b.f32(v);
            b.u8(static_cast<std::uint8_t>(a.state));
            b.u64(a.targetId);
            b.boolean(a.targetIsCreature);
            b.boolean(a.alive);
            b.u8(a.deathCause);
        }
        b.u32(static_cast<std::uint32_t>(f.explored.size()));   // заселённые ячейки мира без границ (в конце: старые файлы без них читаются)
        for (std::int64_t k : f.explored) b.i64(k);
        section(payload, kTagFauna, b);
    }
    {
        ByteWriter b;
        b.u32(static_cast<std::uint32_t>(g.world.structures.size()));
        for (const Structure& s : g.world.structures) { b.u8(static_cast<std::uint8_t>(s.kind)); b.f32(s.x); b.f32(s.z); b.f32(s.yaw); b.f32(s.hp); b.f32(s.stock); }
        section(payload, kTagStructs2, b);
    }
    {
        ByteWriter b;
        const WorldState& w = g.world;
        b.boolean(w.streaming);
        std::uint64_t bits;
        std::memcpy(&bits, &w.time, 8);
        b.u64(bits);
        b.u32(static_cast<std::uint32_t>(w.loadedCells.size()));
        for (std::int64_t k : w.loadedCells) b.i64(k);
        b.u32(static_cast<std::uint32_t>(w.cellRecords.size()));
        for (const CellRecordData& r : w.cellRecords) {
            b.i64(r.key);
            std::memcpy(&bits, &r.lastTime, 8);
            b.u64(bits);
            b.u32(static_cast<std::uint32_t>(r.deltas.size()));
            for (const auto& [local, amount] : r.deltas) { b.u32(static_cast<std::uint32_t>(local)); b.f32(amount); }
            b.u32(static_cast<std::uint32_t>(r.planted.size()));
            for (const FoodSource& f : r.planted) { b.vec3(f.position); b.f32(f.amount); b.u8(static_cast<std::uint8_t>(f.kind)); }
        }
        b.u32(static_cast<std::uint32_t>(w.food.size()));   // происхождение каждого материализованного куста (в порядке WRLD.food)
        for (const FoodSource& f : w.food) { b.u32(static_cast<std::uint32_t>(f.cellX)); b.u32(static_cast<std::uint32_t>(f.cellZ)); b.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(f.local))); }
        section(payload, kTagStream, b);
    }
    {
        ByteWriter b;
        b.u32(static_cast<std::uint32_t>(g.bodies.size()));
        for (const auto& body : g.bodies) {
            b.vec3(body.pos);
            b.vec3(body.vel);
            for (float q : {body.orient.w, body.orient.x, body.orient.y, body.orient.z}) b.f32(q);
            b.f32(body.radius);
            b.f32(body.invMass);
            b.f32(body.restitution);
            b.boolean(body.asleep);
            b.f32(body.sleepTimer);
            b.f32(body.asleepFor);
        }
        section(payload, kTagPhysics, b);
    }
    {
        ByteWriter b;
        b.vec3(g.camera.target);
        b.f32(g.camera.yaw);
        b.f32(g.camera.pitch);
        b.f32(g.camera.distance);
        section(payload, kTagCamera, b);
    }

    ByteWriter meta;
    writeMeta(meta, g.meta);

    ByteWriter out;
    for (char c : kMagic) out.u8(static_cast<std::uint8_t>(c));
    out.u32(version);
    out.u32(static_cast<std::uint32_t>(meta.bytes().size()));
    out.raw(meta.bytes());
    out.u32(static_cast<std::uint32_t>(payload.bytes().size()));
    out.u32(crc32(payload.bytes()));
    out.raw(payload.bytes());
    return out.take();
}

namespace {

// Общий разбор заголовка: магия, версия, сводка. Оставляет читатель сразу за сводкой.
bool parseHeader(ByteReader& r, SaveMeta& meta, std::string& error, std::uint32_t* versionOut = nullptr) {
    const auto magic = r.bytes(8);
    if (!r.ok() || std::memcmp(magic.data(), kMagic, 8) != 0) { error = "это не файл сохранения WorldSimulator"; return false; }
    const std::uint32_t version = r.u32();
    if (!r.ok()) { error = "файл повреждён (обрезан)"; return false; }
    if (version > kSaveVersion) { error = "сохранено более новой версией игры (формат " + std::to_string(version) + ")"; return false; }
    if (version == 0) { error = "файл повреждён"; return false; }
    if (versionOut) *versionOut = version;
    const std::uint32_t metaSize = r.u32();
    if (metaSize > 4096 || metaSize > r.remaining()) { error = "файл повреждён (заголовок)"; return false; }
    ByteReader mr(r.bytes(metaSize));
    meta = readMeta(mr);
    if (!mr.ok()) { error = "файл повреждён (заголовок)"; return false; }
    return true;
}

}  // namespace

bool parseSaveMeta(std::span<const std::uint8_t> bytes, SaveMeta& out, std::string& error) {
    ByteReader r(bytes);
    return parseHeader(r, out, error);
}

bool parseSave(std::span<const std::uint8_t> bytes, SaveGame& out, std::string& error) {
    ByteReader r(bytes);
    SaveGame   g;
    std::uint32_t version = 0;
    if (!parseHeader(r, g.meta, error, &version)) return false;

    const std::uint32_t payloadSize = r.u32(), expectedCrc = r.u32();
    if (!r.ok() || payloadSize > r.remaining()) { error = "файл повреждён (обрезан)"; return false; }
    const auto payload = r.bytes(payloadSize);
    if (crc32(payload) != expectedCrc) { error = "файл повреждён (контрольная сумма не сошлась)"; return false; }

    bool haveTerrain = false, haveWorld = false;
    std::vector<std::uint8_t> berryKinds;
    struct FoodOrigin { std::int32_t cx, cz, local; };
    std::vector<FoodOrigin> streamFood;   // происхождение кустов из STRM (применяется после чтения всех секций)
    ByteReader pr(payload);
    while (pr.remaining() >= 8) {
        const std::uint32_t t = pr.u32(), len = pr.u32();
        if (len > pr.remaining()) { error = "файл повреждён (секция)"; return false; }
        ByteReader sr(pr.bytes(len));
        if (t == kTagTerrain) {
            g.terrainSeed     = sr.u32();
            g.terrainHalfSize = sr.f32();
            const std::uint32_t n = sr.count(kMaxTiles, 12);
            for (std::uint32_t i = 0; i < n && sr.ok(); ++i) {
                Terrain::EditTileData d;
                d.tx     = static_cast<int>(sr.u32());
                d.tz     = static_cast<int>(sr.u32());
                d.values = sr.floats(Terrain::kEditTileSize * Terrain::kEditTileSize);
                g.terrainEdits.push_back(std::move(d));
            }
            if (sr.ok() && sr.remaining() >= 4) {
                const std::uint32_t m = sr.count(1000000, 8);
                for (std::uint32_t i = 0; i < m && sr.ok(); ++i) g.felledTrees.push_back(sr.i64());
            }
            if (sr.ok() && sr.remaining() >= 4) {
                const std::uint32_t m = sr.count(1000000, 8);
                for (std::uint32_t i = 0; i < m && sr.ok(); ++i) g.bridges.push_back(sr.i64());
            }
            haveTerrain = sr.ok();
        } else if (t == kTagWorld) {
            g.world.rng    = sr.str(1 << 16);
            g.world.nextId = sr.u64();
            const std::uint32_t pop = sr.count(kMaxCreatures, 100);
            g.world.population.resize(pop);
            for (std::uint32_t i = 0; i < pop && sr.ok(); ++i)
                if (!readCreature(sr, g.world.population[i], version)) { error = "файл повреждён (существо)"; return false; }
            const std::uint32_t food = sr.count(kMaxFood, 16);
            for (std::uint32_t i = 0; i < food && sr.ok(); ++i) { FoodSource f; f.position = sr.vec3(); f.amount = sr.f32(); g.world.food.push_back(f); }
            const std::uint32_t houses = sr.count(kMaxBuildings, 16);
            for (std::uint32_t i = 0; i < houses && sr.ok(); ++i) { Building h; h.x = sr.f32(); h.z = sr.f32(); h.yaw = sr.f32(); h.seed = sr.u32(); g.world.buildings.push_back(h); }
            const std::uint32_t grief = sr.count(kMaxGrief, 12);
            for (std::uint32_t i = 0; i < grief && sr.ok(); ++i) { const std::uint64_t id = sr.u64(); g.world.pendingGrief.emplace_back(id, sr.f32()); }
            haveWorld = sr.ok();
        } else if (t == kTagPhysics) {
            const std::uint32_t n = sr.count(kMaxBodies, 60);
            for (std::uint32_t i = 0; i < n && sr.ok(); ++i) {
                RigidBody b;
                b.pos = sr.vec3();
                b.vel = sr.vec3();
                const float w = sr.f32(), x = sr.f32(), y = sr.f32(), z = sr.f32();
                b.orient      = glm::normalize(glm::quat(w, x, y, z));
                b.radius      = sr.f32();
                b.invMass     = sr.f32();
                b.restitution = sr.f32();
                b.asleep      = sr.boolean();
                b.sleepTimer  = sr.f32();
                b.asleepFor   = sr.f32();
                g.bodies.push_back(b);
            }
        } else if (t == kTagFauna) {
            Fauna::State& f = g.world.fauna;
            f.nextId = std::max<std::uint64_t>(sr.u64(), 1);
            const float timer = sr.f32();
            f.immigrationTimer = std::isfinite(timer) ? timer : 0.0f;
            const std::uint32_t n = sr.count(Fauna::kMaxAnimals, 81);  // 81 байт на животное
            for (std::uint32_t i = 0; i < n && sr.ok(); ++i) {
                Animal a;
                a.id = sr.u64();
                const std::uint8_t sp = sr.u8();
                a.species  = sp < kSpeciesCount ? static_cast<Species>(sp) : Species::Hare;
                a.position = sr.vec3();
                auto fin = [&](float lo, float hi) { const float v = sr.f32(); return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
                a.heading = fin(-100.0f, 100.0f);
                a.speedNow = fin(0.0f, 50.0f);
                a.hunger = fin(0.0f, 1.0f);
                a.health = fin(0.0f, 1.0f);
                a.age = fin(0.0f, 1.0e6f);
                a.stamina = fin(0.0f, 1.0f);
                a.breedCooldown = fin(0.0f, 1.0e4f);
                a.attackCooldown = fin(0.0f, 100.0f);
                a.searchCooldown = fin(0.0f, 1000.0f);
                a.stateTimer = fin(0.0f, 1000.0f);
                a.wanderTimer = fin(-1000.0f, 1000.0f);
                a.sinceBite = fin(0.0f, 1.0e6f);
                const std::uint8_t st = sr.u8();
                a.state = st < static_cast<std::uint8_t>(AnimalState::Count) ? static_cast<AnimalState>(st) : AnimalState::Wander;
                a.targetId = sr.u64();
                a.targetIsCreature = sr.boolean();
                a.alive = sr.boolean();
                a.deathCause = sr.u8();
                if (!std::isfinite(a.position.x) || !std::isfinite(a.position.z)) { error = "файл повреждён (животное)"; return false; }
                f.animals.push_back(a);
            }
            if (sr.remaining() >= 4) {   // исследованные ячейки (добавлены позже секции)
                const std::uint32_t m = sr.count(500000, 8);
                for (std::uint32_t i = 0; i < m && sr.ok(); ++i) f.explored.push_back(sr.i64());
            }
        } else if (t == kTagStructs || t == kTagStructs2) {   // STRC (v8, без склада) и STR2
            const bool v2 = t == kTagStructs2;
            const std::uint32_t m = sr.count(World::kMaxStructures, v2 ? 21 : 17);
            for (std::uint32_t i = 0; i < m && sr.ok(); ++i) {
                Structure st;
                const std::uint8_t kind = sr.u8();
                st.x = sr.f32(); st.z = sr.f32(); st.yaw = sr.f32(); st.hp = sr.f32();
                if (v2) { const float stock = sr.f32(); st.stock = std::isfinite(stock) ? std::clamp(stock, 0.0f, kStockpileCapacity) : 0.0f; }
                if (kind < 1 || kind > 3) { error = "файл повреждён (постройка)"; return false; }
                if (!std::isfinite(st.x) || !std::isfinite(st.z) || !std::isfinite(st.yaw) || !std::isfinite(st.hp)) { error = "файл повреждён (постройка)"; return false; }
                st.kind = static_cast<StructureKind>(kind);
                g.world.structures.push_back(st);
            }
        } else if (t == kTagStream) {
            WorldState& w = g.world;
            w.streaming = sr.boolean();
            std::uint64_t bits = sr.u64();
            std::memcpy(&w.time, &bits, 8);
            if (!std::isfinite(w.time)) w.time = 0.0;
            const std::uint32_t cells = sr.count(500000, 8);
            for (std::uint32_t i = 0; i < cells && sr.ok(); ++i) w.loadedCells.push_back(sr.i64());
            const std::uint32_t recs = sr.count(500000, 24);
            for (std::uint32_t i = 0; i < recs && sr.ok(); ++i) {
                CellRecordData r;
                r.key = sr.i64();
                bits = sr.u64();
                std::memcpy(&r.lastTime, &bits, 8);
                if (!std::isfinite(r.lastTime)) r.lastTime = 0.0;
                const std::uint32_t nd = sr.count(256, 8);
                for (std::uint32_t k = 0; k < nd && sr.ok(); ++k) {
                    const auto local = static_cast<std::int16_t>(sr.u32());
                    const float amount = sr.f32();
                    r.deltas.emplace_back(local, std::isfinite(amount) ? std::clamp(amount, -1.0f, 1.0f) : -1.0f);
                }
                const std::uint32_t np = sr.count(4000, 17);
                for (std::uint32_t k = 0; k < np && sr.ok(); ++k) {
                    FoodSource f;
                    f.position = sr.vec3();
                    f.amount   = std::clamp(sr.f32(), 0.0f, 1.0f);
                    const std::uint8_t kind = sr.u8();
                    f.kind = kind < kBerryKindCount ? static_cast<BerryKind>(kind) : BerryKind::Raspberry;
                    f.local = -1;
                    f.cellX = static_cast<std::int32_t>(std::floor(f.position.x / kBushCell));
                    f.cellZ = static_cast<std::int32_t>(std::floor(f.position.z / kBushCell));
                    r.planted.push_back(f);
                }
                w.cellRecords.push_back(std::move(r));
            }
            const std::uint32_t nf = sr.count(kMaxFood, 12);
            for (std::uint32_t i = 0; i < nf && sr.ok(); ++i) {
                streamFood.push_back({static_cast<std::int32_t>(sr.u32()), static_cast<std::int32_t>(sr.u32()), static_cast<std::int32_t>(sr.u32())});
            }
        } else if (t == kTagBerries) {
            const std::uint32_t n = sr.count(kMaxFood, 1);
            for (std::uint32_t i = 0; i < n && sr.ok(); ++i) berryKinds.push_back(sr.u8());
        } else if (t == kTagCamera) {
            g.camera.target   = sr.vec3();
            g.camera.yaw      = sr.f32();
            g.camera.pitch    = sr.f32();
            g.camera.distance = sr.f32();
        }  // незнакомая секция (более новая версия формата) -- пропущена
        if (!sr.ok()) { error = "файл повреждён (данные секции)"; return false; }
    }
    if (!haveTerrain || !haveWorld) { error = "в файле нет рельефа или мира"; return false; }
    // Происхождение кустов (потоковый мир): секция должна сойтись по числу кустов, иначе считаем файл старым (кусты -- "посаженные").
    if (g.world.streaming && streamFood.size() == g.world.food.size())
        for (std::size_t i = 0; i < streamFood.size(); ++i) {
            g.world.food[i].cellX = streamFood[i].cx;
            g.world.food[i].cellZ = streamFood[i].cz;
            g.world.food[i].local = static_cast<std::int16_t>(std::clamp(streamFood[i].local, -1, 32000));
        }
    else g.world.streaming = false;
    // Виды ягод: если секция есть и сходится по числу кустов -- применяем (неизвестный номер -- малина).
    if (berryKinds.size() == g.world.food.size())
        for (std::size_t i = 0; i < berryKinds.size(); ++i)
            g.world.food[i].kind = berryKinds[i] < kBerryKindCount ? static_cast<BerryKind>(berryKinds[i]) : BerryKind::Raspberry;
    out = std::move(g);
    return true;
}

bool restoreSave(const SaveGame& save, Terrain& terrain, World& world, PhysicsWorld& physics) {
    Terrain t(save.terrainHalfSize, save.terrainSeed);
    if (!t.importEdits(save.terrainEdits)) return false;
    t.importFelled(save.felledTrees);
    t.importBridges(save.bridges);
    World w(0);
    if (!w.importState(save.world)) return false;
    // Всё прошло -- подменяем (иначе живой мир остаётся нетронутым).
    terrain = std::move(t);
    world   = std::move(w);
    physics.setBodies(save.bodies);
    return true;
}

// ─── SaveStore ───────────────────────────────────────────────────────────────
bool SaveStore::validSlot(const std::string& slot) {
    if (slot.empty() || slot.size() > 80) return false;
    for (char c : slot)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

std::filesystem::path SaveStore::pathFor(const std::string& slot) const { return m_dir / (slot + ".wsim"); }

bool SaveStore::exists(const std::string& slot) const {
    std::error_code ec;
    return validSlot(slot) && std::filesystem::exists(pathFor(slot), ec);
}

bool SaveStore::save(const std::string& slot, const SaveGame& game, std::string& error) const {
    namespace fs = std::filesystem;
    if (!validSlot(slot)) { error = "недопустимое имя сохранения"; return false; }
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    const auto bytes = serializeSave(game);
    const fs::path tmp = pathFor(slot).string() + ".tmp", dst = pathFor(slot);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "не удалось создать файл в " + m_dir.string(); return false; }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) { fs::remove(tmp, ec); error = "не удалось записать файл (нет места?)"; return false; }
    }
    fs::rename(tmp, dst, ec);  // на одной файловой системе замена атомарна
    if (ec) { fs::remove(tmp, ec); error = "не удалось заменить файл сохранения"; return false; }
    return true;
}

namespace {
bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto size = in.tellg();
    if (size < 0 || size > (std::streamoff{512} << 20)) return false;  // больше 512 МБ -- не наш файл
    out.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}
}  // namespace

bool SaveStore::load(const std::string& slot, SaveGame& out, std::string& error) const {
    if (!validSlot(slot)) { error = "недопустимое имя сохранения"; return false; }
    std::vector<std::uint8_t> bytes;
    if (!readFile(pathFor(slot), bytes)) { error = "не удалось прочитать файл"; return false; }
    return parseSave(bytes, out, error);
}

bool SaveStore::remove(const std::string& slot) const {
    std::error_code ec;
    return validSlot(slot) && std::filesystem::remove(pathFor(slot), ec);
}

std::vector<SaveEntry> SaveStore::list() const {
    namespace fs = std::filesystem;
    std::vector<SaveEntry> out;
    std::error_code ec;
    if (!fs::is_directory(m_dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(m_dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".wsim") continue;
        SaveEntry s;
        s.path = e.path();
        s.slot = e.path().stem().string();
        std::vector<std::uint8_t> bytes;
        if (!validSlot(s.slot)) continue;
        if (!readFile(s.path, bytes)) { s.error = "не удалось прочитать файл"; }
        else s.valid = parseSaveMeta(bytes, s.meta, s.error);
        if (!s.valid && s.meta.name.empty()) s.meta.name = s.slot;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) {
        return a.meta.unixTime != b.meta.unixTime ? a.meta.unixTime > b.meta.unixTime : a.slot < b.slot;
    });
    return out;
}

}  // namespace wsim
