#include "core/TerrainChunks.hpp"

#include "procgen/TreeGen.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace wsim {

namespace {

// Сколько кадров держим выгруженный меш до destroy() (кадров в полёте меньше).
constexpr int kRetireFrames = 6;

// Фиксированный диапазон высот для покраски: чанки строятся независимо,
// так что нормировать по min/max чанка нельзя -- цвет прыгал бы на швах.
constexpr float kColorLow  = -3.0f;
constexpr float kColorHigh = 6.0f;

// Узел уровня L делится на четыре, пока он ближе kSplitFactor * его размер.
constexpr float kSplitFactor = 4.0f;

}  // namespace

TerrainChunks::TerrainChunks(VulkanContext& context, const Terrain& terrain)
    : m_context(context), m_terrain(terrain) {
    // Шаблоны деревьев (по виду и варианту) строим один раз; в чанки они вшиваются с поворотом и масштабом.
    for (auto species : {TreeSpecies::Oak, TreeSpecies::Birch})
        for (int v = 0; v < kTreeVariants; ++v) m_treeTemplates.push_back(procgen::buildTree(species, v));

    // Общая вода: единичный квадрат на уровне моря, каждый чанк только задаёт Transform.
    const glm::vec4 up{0.f, 1.f, 0.f, 0.f};
    const glm::vec4 color{0.16f, 0.35f, 0.55f, 0.5f};  // alpha 0.5 -- материал воды
    const float     y = Terrain::kSeaLevel;
    const std::vector<Vertex> wv = {
        {{0.f, y, 0.f, 0.f}, up, color}, {{1.f, y, 0.f, 0.f}, up, color},
        {{1.f, y, 1.f, 0.f}, up, color}, {{0.f, y, 1.f, 0.f}, up, color},
    };
    const std::vector<std::uint32_t> wi = {0, 2, 1, 2, 0, 3};  // CCW сверху (+Y)
    m_waterMesh = std::make_shared<Mesh>(Mesh::create(
        m_context.physDevice(), m_context.device(), m_context.commandPool(), m_context.graphicsQueue(), wv, wi));
}

TerrainChunks::~TerrainChunks() {
    reset();
    for (auto& r : m_retired) r.mesh->destroy(m_context.device());
    m_waterMesh->destroy(m_context.device());
}

void TerrainChunks::retire(Chunk& chunk) {
    if (chunk.terrain) m_retired.push_back({std::move(chunk.terrain), kRetireFrames});
    if (chunk.trees)   m_retired.push_back({std::move(chunk.trees), kRetireFrames});
}

void TerrainChunks::reset() {
    for (auto& [k, chunk] : m_chunks) retire(chunk);
    for (auto& [id, c] : m_batchBuilt) retire(c);
    m_batchBuilt.clear();
    m_batchPending.clear();
    m_chunks.clear();
    m_dirty.clear();
    m_objects.clear();
    m_casters.clear();
    m_treeCasters.clear();
}

// Деревья чанка одним мешем: корни из Trees::inRect (детерминированно по сиду, по текущему рельефу и без мест,
// занятых домами и кустами), шаблон поворачивается, масштабируется и ставится на землю. Только у самого
// подробного уровня: дальше деревья в сотни тысяч вершин не нужны (и гасятся туманом).
std::shared_ptr<Mesh> TerrainChunks::buildTrees(const Key& key) {
    if (key.level != 0) return nullptr;
    const Trees::Blocker* blocker = m_treeBlocker ? &m_treeBlocker : nullptr;
    const auto trees = Trees::inRect(m_terrain, key.x0(), key.z0(), key.x0() + key.size(), key.z0() + key.size(), blocker);
    if (trees.empty()) return nullptr;

    std::vector<Vertex>        verts;
    std::vector<std::uint32_t> indices;
    for (const TreeInstance& t : trees) {
        const procgen::MeshData& tpl = m_treeTemplates[static_cast<std::size_t>(t.species) * kTreeVariants + t.variant];
        const float yaw = glm::radians(t.yaw), cy = std::cos(yaw), sy = std::sin(yaw);
        const float gy = m_terrain.heightAt(t.x, t.z);
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (const Vertex& v : tpl.vertices) {
            const glm::vec3 p = glm::vec3(v.position) * t.scale;
            const glm::vec3 n = glm::vec3(v.normal);
            Vertex out = v;
            out.position = {t.x + p.x * cy + p.z * sy, gy + p.y, t.z - p.x * sy + p.z * cy, 0.0f};
            out.normal   = {n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy, 0.0f};
            verts.push_back(out);
        }
        for (std::uint32_t i : tpl.indices) indices.push_back(base + i);
    }
    return std::make_shared<Mesh>(Mesh::create(m_context.physDevice(), m_context.device(), m_context.commandPool(),
                                               m_context.graphicsQueue(), verts, indices));
}

TerrainChunks::Chunk TerrainChunks::makeChunk(const Key& key) {
    constexpr int res = kCells + 1;
    const float cell = key.size() / static_cast<float>(kCells);  // шаг сетки этого уровня
    const float x0 = key.x0(), z0 = key.z0();

    const glm::vec4 lowColor{0.24f, 0.32f, 0.22f, 1.f};   // низина -- трава
    const glm::vec4 highColor{0.55f, 0.52f, 0.45f, 1.f};  // высота -- камень/сушь

    std::vector<Vertex> verts;
    verts.reserve(static_cast<std::size_t>(res) * res + 4 * kCells * 2);
    float minH = 1e9f;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const float x = x0 + static_cast<float>(i) * cell;
            const float z = z0 + static_cast<float>(j) * cell;
            const float h = m_terrain.heightAt(x, z);
            minH = std::min(minH, h);

            // Центральные разности по самой heightAt() -- на шве чанков одного уровня одинаковы по обе стороны.
            const float dx = (m_terrain.heightAt(x + cell, z) - m_terrain.heightAt(x - cell, z)) / (2.f * cell);
            const float dz = (m_terrain.heightAt(x, z + cell) - m_terrain.heightAt(x, z - cell)) / (2.f * cell);
            const glm::vec3 n = glm::normalize(glm::vec3(-dx, 1.0f, -dz));

            const float t = std::clamp((h - kColorLow) / (kColorHigh - kColorLow), 0.0f, 1.0f);
            // alpha = код материала для шейдера (0.25 -- рельеф, см. materials.glsl); цвет вершины там не используется.
            verts.push_back({{x, h, z, 0.f}, {n, 0.f}, glm::vec4(glm::vec3(glm::mix(lowColor, highColor, t)), 0.25f)});
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(kCells) * kCells * 6 + 4 * kCells * 6);
    for (int j = 0; j < kCells; ++j) {
        for (int i = 0; i < kCells; ++i) {
            const auto a = static_cast<std::uint32_t>(j * res + i);
            const auto b = a + 1;
            const auto d = a + res;
            const auto c = d + 1;
            indices.insert(indices.end(), {a, c, b, c, a, d});  // CCW сверху (+Y)
        }
    }

    // Юбки: полоса вниз по каждому краю закрывает щели между соседями другого уровня.
    const float drop = cell * 1.5f;
    auto skirt = [&](std::uint32_t va, std::uint32_t vb, const glm::vec3& outward) {
        const auto da = static_cast<std::uint32_t>(verts.size());
        Vertex a = verts[va], b = verts[vb];
        a.position.y -= drop;
        b.position.y -= drop;
        verts.push_back(a);
        verts.push_back(b);
        const std::uint32_t db = da + 1;
        // Порядок по правилу правой руки: нормаль треугольника должна смотреть наружу.
        const glm::vec3 pa(verts[va].position), pb(verts[vb].position), pdb(verts[db].position);
        if (glm::dot(glm::cross(pb - pa, pdb - pa), outward) > 0.0f) indices.insert(indices.end(), {va, vb, db, va, db, da});
        else                                                           indices.insert(indices.end(), {va, db, vb, va, da, db});
    };
    for (int k = 0; k < kCells; ++k) {
        const auto K = static_cast<std::uint32_t>(k);
        skirt(K, K + 1, {0.f, 0.f, -1.f});                                                       // z = z0
        skirt(static_cast<std::uint32_t>(kCells * res) + K, static_cast<std::uint32_t>(kCells * res) + K + 1, {0.f, 0.f, 1.f});  // z = z0 + size
        skirt(K * res, (K + 1) * res, {-1.f, 0.f, 0.f});                                         // x = x0
        skirt(K * res + kCells, (K + 1) * res + kCells, {1.f, 0.f, 0.f});                        // x = x0 + size
    }

    Chunk chunk;
    chunk.key     = key;
    chunk.water   = minH < Terrain::kSeaLevel;
    chunk.trees   = buildTrees(key);
    chunk.terrain = std::make_shared<Mesh>(Mesh::create(
        m_context.physDevice(), m_context.device(), m_context.commandPool(), m_context.graphicsQueue(),
        verts, indices));
    return chunk;
}

void TerrainChunks::build(const Key& key) { m_chunks.emplace(key.id(), makeChunk(key)); }

void TerrainChunks::invalidate(const Terrain::Rect& r) {
    for (const auto& [id, chunk] : m_chunks) {
        const Key& k = chunk.key;
        if (k.x0() < r.x1 && r.x0 < k.x0() + k.size() && k.z0() < r.z1 && r.z0 < k.z0() + k.size()) m_dirty.insert(id);
    }
}

// Квадродерево: рекурсивно делим узлы, пока они ближе kSplitFactor * размер; листья -- нужные чанки.
void TerrainChunks::collect(const Key& node, float cx, float cz, float range, std::vector<Key>& out) const {
    const float s  = node.size();
    const float nx = std::clamp(cx, node.x0(), node.x0() + s);
    const float nz = std::clamp(cz, node.z0(), node.z0() + s);
    const float dist = std::hypot(cx - nx, cz - nz);
    if (dist > range) return;
    if (node.level > 0 && dist < kSplitFactor * s) {
        for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx)
                collect({node.level - 1, node.cx * 2 + dx, node.cz * 2 + dz}, cx, cz, range, out);
    } else {
        out.push_back(node);
    }
}

void TerrainChunks::update(const glm::vec3& center, int maxBuilds, int maxRebuilds) {
    // Освободить созданные давно выгруженные меши.
    for (auto& r : m_retired) --r.framesLeft;
    std::erase_if(m_retired, [&](Retired& r) {
        if (r.framesLeft > 0) return false;
        r.mesh->destroy(m_context.device());
        return true;
    });

    // Нужный набор листьев.
    const float range = static_cast<float>(m_viewRadius) * kChunkSize;
    std::vector<Key> desired;
    {
        const float rootSize = kChunkSize * static_cast<float>(1 << kMaxLevel);
        const int   minX = static_cast<int>(std::floor((center.x - range) / rootSize)), maxX = static_cast<int>(std::floor((center.x + range) / rootSize));
        const int   minZ = static_cast<int>(std::floor((center.z - range) / rootSize)), maxZ = static_cast<int>(std::floor((center.z + range) / rootSize));
        for (int z = minZ; z <= maxZ; ++z)
            for (int x = minX; x <= maxX; ++x) collect({kMaxLevel, x, z}, center.x, center.z, range, desired);
    }
    std::unordered_set<std::int64_t> desiredSet;
    desiredSet.reserve(desired.size() * 2);
    for (const Key& k : desired) desiredSet.insert(k.id());

    // Достроить недостающее: ближние первыми. Пустая карта -- строим всё сразу (старт без дыр).
    std::vector<std::pair<float, Key>> missing;
    for (const Key& k : desired)
        if (!m_chunks.contains(k.id()))
            missing.push_back({std::hypot(k.x0() + k.size() * 0.5f - center.x, k.z0() + k.size() * 0.5f - center.z), k});
    std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::size_t budget = m_chunks.empty() ? missing.size() : static_cast<std::size_t>(maxBuilds);
    m_buildsLast = static_cast<int>(std::min(budget, missing.size()));
    for (std::size_t i = 0; i < missing.size() && i < budget; ++i) build(missing[i].second);

    // Пересборка чанков после правок рельефа -- пачкой: ближние к камере первыми,
    // по бюджету на кадр, но заменяем на экране все разом, когда готова вся пачка.
    if (m_batchPending.empty() && m_batchBuilt.empty() && !m_dirty.empty()) {
        std::vector<std::pair<float, std::int64_t>> order;
        for (auto id : m_dirty) {
            const auto it = m_chunks.find(id);
            if (it == m_chunks.end()) continue;
            const Key& k = it->second.key;
            order.push_back({std::hypot(k.x0() + k.size() * 0.5f - center.x, k.z0() + k.size() * 0.5f - center.z), id});
        }
        m_dirty.clear();
        std::sort(order.begin(), order.end());
        for (const auto& o : order) m_batchPending.push_back(o.second);
    }
    for (int i = 0; i < maxRebuilds && !m_batchPending.empty(); ++i) {
        const std::int64_t id = m_batchPending.front();
        m_batchPending.erase(m_batchPending.begin());
        const auto it = m_chunks.find(id);
        if (it == m_chunks.end()) continue;  // уже выгружен
        m_batchBuilt.emplace_back(id, makeChunk(it->second.key));
        ++m_buildsLast;
    }
    if (m_batchPending.empty() && !m_batchBuilt.empty()) {
        for (auto& [id, fresh] : m_batchBuilt) {
            const auto it = m_chunks.find(id);
            if (it == m_chunks.end()) { retire(fresh); continue; }
            retire(it->second);
            it->second = std::move(fresh);
        }
        m_batchBuilt.clear();
    }

    // Лишние (не нужные сейчас) чанки: убираем, когда всё, что их заменяет, уже построено.
    std::vector<Key> loadedDesired;
    for (const Key& k : desired)
        if (m_chunks.contains(k.id())) loadedDesired.push_back(k);
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        if (desiredSet.contains(it->first)) { ++it; continue; }
        bool replaced = true;
        for (const Key& d : desired)
            if (it->second.key.overlaps(d) && !m_chunks.contains(d.id())) { replaced = false; break; }
        if (replaced) {
            retire(it->second);
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }

    // Новые чанки скрыты, пока под ними виден старый (иначе z-fight на перекрытии).
    std::vector<bool> hidden(loadedDesired.size(), false);
    for (const auto& [id, chunk] : m_chunks) {
        if (desiredSet.contains(id)) continue;
        for (std::size_t i = 0; i < loadedDesired.size(); ++i)
            if (chunk.key.overlaps(loadedDesired[i])) hidden[i] = true;
    }
    std::unordered_set<std::int64_t> hiddenIds;
    for (std::size_t i = 0; i < loadedDesired.size(); ++i)
        if (hidden[i]) hiddenIds.insert(loadedDesired[i].id());

    m_objects.clear();
    m_casters.clear();
    m_treeCasters.clear();
    for (const auto& [id, chunk] : m_chunks) {
        if (hiddenIds.contains(id)) continue;
        m_objects.push_back({chunk.terrain, Transform{}, /*solid=*/false});
        m_casters.push_back({chunk.terrain, Transform{}, /*solid=*/false});
        if (chunk.trees) {
            m_objects.push_back({chunk.trees, Transform{}, /*solid=*/false});
            m_treeCasters.push_back({chunk.trees, Transform{}, /*solid=*/false});
        }
        if (chunk.water) {
            Transform t;
            t.position = {chunk.key.x0(), 0.f, chunk.key.z0()};
            t.scale    = {chunk.key.size(), 1.f, chunk.key.size()};
            m_objects.push_back({m_waterMesh, t, /*solid=*/false});
        }
    }
}

}  // namespace wsim
