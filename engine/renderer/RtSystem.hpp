#pragma once

#include "renderer/GpuBuffer.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wsim {

// Инкапсулирует VK_KHR_ray_query: BLAS на уникальный меш + TLAS сцены + таблица
// геометрии (адреса vbo/ibo на инстанс) для hit-shading в GI/отражениях.
// BLAS и geo-table статичны (строятся один раз). TLAS двойной буферизации — по
// одному на кадр-в-полёте: `updateInstances` перестраивает TLAS текущего кадра с
// новыми трансформами (динамика двигает объекты), без гонок между кадрами.
// Дескрипторы (set 0, binding 2 = TLAS, binding 3 = geo-table) пишет владелец
// (VulkanContext) — по одному tlas(frame) на кадровый дескриптор-сет.
class RtSystem {
public:
    struct Instance {
        VkBuffer    vbo;
        VkBuffer    ibo;
        uint32_t    vertexCount;
        uint32_t    indexCount;
        glm::mat4   model;
        const void* meshKey;  // общий ключ инстансов одного меша (BLAS reuse)
    };

    // supported — включены ли RT-расширения (решается при создании девайса);
    // при true грузит указатели функций acceleration structure.
    // frameCount — число кадров-в-полёте (сколько независимых TLAS держать).
    void init(VkDevice device, VkPhysicalDevice phys, bool supported, uint32_t frameCount);
    void destroy();

    // Строит BLAS + geo-table + начальные TLAS всех кадров одним submit.
    void build(VkQueue queue, VkCommandPool pool, const std::vector<Instance>& instances);

    // Обновляет TLAS кадра `frame` под новые трансформы инстансов (в порядке
    // build). No-op, если трансформы не изменились И слот уже актуален — иначе
    // пишет инстанс-буфер, записывает build в cmd + барьер AS-build → ray query.
    // Вызывать в кадровом cmd ДО scene/RT-пасса.
    void updateInstances(VkCommandBuffer cmd, uint32_t frame,
                         const std::vector<glm::mat4>& models);

    [[nodiscard]] bool supported() const noexcept { return m_supported; }
    [[nodiscard]] bool ready() const noexcept {
        return !m_frames.empty() && m_frames[0].tlas != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkAccelerationStructureKHR tlas(uint32_t frame) const noexcept {
        return m_frames[frame].tlas;
    }
    [[nodiscard]] VkBuffer     geoTable() const noexcept { return m_geoTableBuffer.handle; }
    [[nodiscard]] VkDeviceSize geoTableSize() const noexcept { return m_geoTableSize; }

private:
    void recordTlasBuild(VkCommandBuffer cmd, uint32_t frame);  // BUILD в dst=tlas[frame]

    VkDevice         m_device     = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys       = VK_NULL_HANDLE;
    bool             m_supported  = false;
    uint32_t         m_frameCount = 0;

    struct AccelStructure {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        GpuBuffer                  buffer;  // память самой AS
    };
    std::vector<AccelStructure> m_blas;  // по одному на уникальный меш (статичны)

    // TLAS кадра-в-полёте: своя AS + инстанс-буфер (host-visible) + scratch.
    // version — версия трансформов, зашитая в этот TLAS (для dirty-skip).
    struct FrameTlas {
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        GpuBuffer                  buffer;          // память TLAS
        GpuBuffer                  instanceBuffer;  // host-visible, persistently mapped
        void*                      instMapped = nullptr;
        GpuBuffer                  scratch;         // build scratch (персистентный)
        uint64_t                   version = 0;     // == m_transformVersion при сборке
    };
    std::vector<FrameTlas> m_frames;

    // CPU-шаблон инстансов (BLAS-ссылки, customIndex, флаги; трансформы патчатся).
    std::vector<VkAccelerationStructureInstanceKHR> m_instTemplate;
    uint32_t     m_instCount = 0;

    // Dirty-check: последние загруженные трансформы + их версия. updateInstances
    // трогает шаблон и поднимает версию только при реальном изменении; слот
    // пересобирается лишь когда его version отстаёт от m_transformVersion.
    std::vector<glm::mat4> m_lastModels;
    uint64_t               m_transformVersion = 0;

    GpuBuffer    m_geoTableBuffer;  // per-instance {vboAddr, iboAddr} (статична)
    VkDeviceSize m_geoTableSize = 0;

    PFN_vkCreateAccelerationStructureKHR           m_fpCreateAS        = nullptr;
    PFN_vkDestroyAccelerationStructureKHR          m_fpDestroyAS       = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR    m_fpGetASBuildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR        m_fpCmdBuildAS      = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR m_fpGetASAddress    = nullptr;
};

}  // namespace wsim
