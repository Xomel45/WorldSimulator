#pragma once

#include "renderer/SceneObject.hpp"
#include "procgen/MeshBuilder.hpp"
#include "sim/Terrain.hpp"
#include "sim/Trees.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wsim {

class VulkanContext;

// Бесконечный рельеф чанками с уровнями детализации (LOD). Terrain::heightAt()
// -- чистая функция, так что чанк можно построить в любой момент в любом месте.
//
// Чанк уровня L покрывает (kChunkSize << L) юнитов и всегда имеет 16x16 ячеек,
// то есть вдали сетка грубее (шаг 2, 4, 8, 16 юнитов), а число чанков остаётся
// небольшим даже на дальности 64 чанков (~2000 юнитов): плоская сетка дала бы
// ~12 800 чанков, а это десятки тысяч VkDeviceMemory (лимит драйвера ~4096).
// Набор нужных чанков -- квадродерево вокруг камеры: узел делится, пока
// ближе (4 * его размер). Листья тайлят мир без нахлёстов; щели на стыках
// разных уровней закрыты юбками (вертикальные полосы вниз по краю чанка).
//
// Смена набора идёт без дыр: старый чанк (уже на экране) убирается, только
// когда все новые под ним построены, а пока он виден, новые под ним скрыты.
class TerrainChunks {
public:
    static constexpr float kChunkSize = 32.0f;  // сторона чанка уровня 0
    static constexpr int   kCells     = 16;     // ячеек на сторону у чанка любого уровня
    static constexpr int   kMaxLevel  = 3;      // уровни 0..3 (чанк до 256 юнитов)

    TerrainChunks(VulkanContext& context, const Terrain& terrain);
    ~TerrainChunks();

    TerrainChunks(const TerrainChunks&)            = delete;
    TerrainChunks& operator=(const TerrainChunks&) = delete;

    // Раз в кадр: подобрать нужный набор вокруг center, достроить недостающее
    // (не больше maxBuilds за вызов, ближние первыми), убрать ненужное.
    // Выгруженные меши освобождаются с задержкой в несколько кадров -- GPU ещё
    // может рисовать ими предыдущие кадры.
    void update(const glm::vec3& center, int maxBuilds = 4, int maxRebuilds = 6);

    // Дальность в чанках уровня 0 (4..64): 64 -- около 2000 юнитов.
    void setViewRadius(int radius) noexcept { m_viewRadius = radius; }
    [[nodiscard]] int viewRadius() const noexcept { return m_viewRadius; }

    // Сколько чанков построено в последнем update() (стройка тормозит кадр --
    // автоподбор дальности не должен принимать это за нехватку мощности).
    [[nodiscard]] int buildsLastUpdate() const noexcept { return m_buildsLast; }

    // Рельеф под прямоугольником изменился (сила бога): перестроить затронутые
    // чанки. Пересборка идёт пачкой по несколько чанков за кадр, но на экране
    // все чанки пачки заменяются одновременно, а до того виден целиком старый
    // рельеф: иначе на стыке обновлённого чанка и ещё старого соседа открывается
    // щель выше глубины юбки.
    void invalidate(const Terrain::Rect& rect);

    // Где деревья не растут (дома, кусты с едой). Вызывается при построении чанка; поменялось -- invalidate().
    void setTreeBlocker(Trees::Blocker blocker) { m_treeBlocker = std::move(blocker); }

    // Выбросить всё (новый сид рельефа).
    void reset();

    [[nodiscard]] const std::vector<SceneObject>& objects() const noexcept { return m_objects; }
    // Только рельеф (без воды): то, что отбрасывает тени.
    [[nodiscard]] const std::vector<SceneObject>& shadowCasters() const noexcept { return m_casters; }
    // Деревья отдельно от рельефа: далёкие каскады теней их не рисуют (на таких расстояниях тень дерева не видна).
    [[nodiscard]] const std::vector<SceneObject>& treeCasters() const noexcept { return m_treeCasters; }
    [[nodiscard]] std::size_t chunkCount() const noexcept { return m_chunks.size(); }

private:
    struct Key {
        int level = 0, cx = 0, cz = 0;
        [[nodiscard]] float size() const noexcept { return kChunkSize * static_cast<float>(1 << level); }
        [[nodiscard]] float x0() const noexcept { return static_cast<float>(cx) * size(); }
        [[nodiscard]] float z0() const noexcept { return static_cast<float>(cz) * size(); }
        [[nodiscard]] std::int64_t id() const noexcept {
            return (static_cast<std::int64_t>(level) << 60) ^ ((static_cast<std::int64_t>(cx) & 0x3fffffff) << 30) ^
                   (static_cast<std::int64_t>(cz) & 0x3fffffff);
        }
        [[nodiscard]] bool overlaps(const Key& o) const noexcept {
            return x0() < o.x0() + o.size() && o.x0() < x0() + size() && z0() < o.z0() + o.size() && o.z0() < z0() + size();
        }
    };
    struct Chunk {
        Key                   key;
        std::shared_ptr<Mesh> terrain;
        std::shared_ptr<Mesh> trees;   // деревья чанка (только у самого подробного уровня), вшиты в один меш
        bool                  water = false;  // есть вершины ниже уровня моря
    };
    struct Retired {
        std::shared_ptr<Mesh> mesh;
        int                   framesLeft;
    };

    void collect(const Key& node, float cx, float cz, float range, std::vector<Key>& out) const;
    Chunk makeChunk(const Key& key);
    std::shared_ptr<Mesh> buildTrees(const Key& key);
    void  build(const Key& key);
    void retire(Chunk& chunk);

    VulkanContext&                          m_context;
    const Terrain&                          m_terrain;
    std::unordered_map<std::int64_t, Chunk> m_chunks;
    std::vector<Retired>                    m_retired;
    std::unordered_set<std::int64_t>        m_dirty;  // ждут следующей пачки пересборки после правки рельефа
    // Текущая пачка: что ещё строить и что уже построено (меняется на экране разом, когда готово всё).
    std::vector<std::int64_t>               m_batchPending;
    std::vector<std::pair<std::int64_t, Chunk>> m_batchBuilt;
    std::vector<SceneObject>                m_objects;
    std::vector<SceneObject>                m_casters;
    std::vector<SceneObject>                m_treeCasters;
    std::shared_ptr<Mesh>                   m_waterMesh;  // единичный квадрат, масштабируется под чанк
    int                                     m_viewRadius = 8;
    int                                     m_buildsLast = 0;
    Trees::Blocker                          m_treeBlocker;
    std::vector<procgen::MeshData>          m_treeTemplates;  // [вид * kTreeVariants + вариант]
};

}  // namespace wsim
