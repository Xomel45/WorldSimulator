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
