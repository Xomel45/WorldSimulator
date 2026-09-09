#include "renderer/RtSystem.hpp"

#include "core/Logger.hpp"
#include "renderer/Vertex.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace wsim {

// ─── init ─────────────────────────────────────────────────────────────────────
void RtSystem::init(VkDevice device, VkPhysicalDevice phys, bool supported, uint32_t frameCount) {
    m_device     = device;
    m_phys       = phys;
    m_supported  = supported;
    m_frameCount = frameCount;
    if (!supported) return;

    const auto load = [&](const char* name) { return vkGetDeviceProcAddr(m_device, name); };
    m_fpCreateAS        = (PFN_vkCreateAccelerationStructureKHR)load("vkCreateAccelerationStructureKHR");
    m_fpDestroyAS       = (PFN_vkDestroyAccelerationStructureKHR)load("vkDestroyAccelerationStructureKHR");
    m_fpGetASBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)load("vkGetAccelerationStructureBuildSizesKHR");
    m_fpCmdBuildAS      = (PFN_vkCmdBuildAccelerationStructuresKHR)load("vkCmdBuildAccelerationStructuresKHR");
    m_fpGetASAddress    = (PFN_vkGetAccelerationStructureDeviceAddressKHR)load("vkGetAccelerationStructureDeviceAddressKHR");
}

// ─── destroy ──────────────────────────────────────────────────────────────────
void RtSystem::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& f : m_frames) {
        if (f.tlas != VK_NULL_HANDLE) m_fpDestroyAS(m_device, f.tlas, nullptr);
        if (f.instMapped) f.instanceBuffer.unmap(m_device);
        f.buffer.destroy(m_device);
        f.instanceBuffer.destroy(m_device);
        f.scratch.destroy(m_device);
    }
    m_frames.clear();
    m_geoTableBuffer.destroy(m_device);
    for (auto& as : m_blas) {
        m_fpDestroyAS(m_device, as.handle, nullptr);
        as.buffer.destroy(m_device);
    }
    m_blas.clear();
    m_instTemplate.clear();
}

// ─── recordTlasBuild ──────────────────────────────────────────────────────────
// Полная сборка (MODE_BUILD) TLAS кадра `frame` из его инстанс-буфера и scratch.
void RtSystem::recordTlasBuild(VkCommandBuffer cmd, uint32_t frame) {
    FrameTlas& F = m_frames[frame];

    VkAccelerationStructureGeometryKHR tlasGeom{
        .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry     = {.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data  = {.deviceAddress = F.instanceBuffer.address(m_device)},
        }},
    };
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure  = F.tlas,
        .geometryCount = 1,
        .pGeometries   = &tlasGeom,
        .scratchData   = {.deviceAddress = F.scratch.address(m_device)},
    };
    VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = m_instCount};
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    m_fpCmdBuildAS(cmd, 1, &tlasBuild, &pRange);
}

// ─── build ────────────────────────────────────────────────────────────────────
// BLAS на каждый уникальный меш (статичны) + geo-table + начальные TLAS всех
// кадров-в-полёте, одним submit после загрузки сцены.
void RtSystem::build(VkQueue queue, VkCommandPool pool, const std::vector<Instance>& instances) {
    if (!m_supported || instances.empty()) return;

    // Транзиентный командный буфер для one-time build
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAI{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(m_device, &cmdAI, &cmd);
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);

    // ── BLAS: по одному на уникальный meshKey ─────────────────────────────────
    std::unordered_map<const void*, size_t> blasIndex;  // meshKey → индекс в m_blas
    std::vector<GpuBuffer> scratchBuffers;

    for (const auto& inst : instances) {
        if (blasIndex.contains(inst.meshKey)) continue;

        VkBufferDeviceAddressInfo vboInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = inst.vbo};
        VkBufferDeviceAddressInfo iboInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = inst.ibo};

        VkAccelerationStructureGeometryKHR geom{
            .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry     = {.triangles = {
                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,  // Vertex::position.xyz
                .vertexData   = {.deviceAddress = vkGetBufferDeviceAddress(m_device, &vboInfo)},
                .vertexStride = sizeof(Vertex),
                .maxVertex    = inst.vertexCount - 1,
                .indexType    = VK_INDEX_TYPE_UINT32,
                .indexData    = {.deviceAddress = vkGetBufferDeviceAddress(m_device, &iboInfo)},
            }},
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        };

        const uint32_t primCount = inst.indexCount / 3;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries   = &geom,
        };
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        m_fpGetASBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                            &buildInfo, &primCount, &sizes);

        AccelStructure as;
        as.buffer = GpuBuffer::createDeviceLocalEmpty(
            m_phys, m_device, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        VkAccelerationStructureCreateInfoKHR asCI{
            .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = as.buffer.handle,
            .size   = sizes.accelerationStructureSize,
            .type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        };
        if (m_fpCreateAS(m_device, &asCI, nullptr, &as.handle) != VK_SUCCESS)
            throw std::runtime_error("vkCreateAccelerationStructureKHR (BLAS) failed");

        GpuBuffer scratch = GpuBuffer::createDeviceLocalEmpty(
            m_phys, m_device, sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        buildInfo.dstAccelerationStructure  = as.handle;
        buildInfo.scratchData.deviceAddress = scratch.address(m_device);

        VkAccelerationStructureBuildRangeInfoKHR range{.primitiveCount = primCount};
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        m_fpCmdBuildAS(cmd, 1, &buildInfo, &pRange);

        blasIndex.emplace(inst.meshKey, m_blas.size());
        m_blas.push_back(as);
        scratchBuffers.push_back(scratch);
    }

    // Барьер: BLAS build → TLAS build (читает BLAS)
    VkMemoryBarrier2 asBarrier{
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    };
    VkDependencyInfo dep{
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &asBarrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    // ── Шаблон инстансов + таблица геометрии для hit-shading ──────────────────
    // instanceCustomIndex = индекс в geoTable → шейдер по попаданию достаёт
    // адреса vbo/ibo меша и интерполирует нормаль/albedo (GI, отражения).
    struct GeoEntry { VkDeviceAddress vbo; VkDeviceAddress ibo; };
    std::vector<GeoEntry> geoTable;
    geoTable.reserve(instances.size());

    m_instTemplate.clear();
    m_instTemplate.reserve(instances.size());
    for (const auto& inst : instances) {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = m_blas[blasIndex.at(inst.meshKey)].handle,
        };
        VkAccelerationStructureInstanceKHR vkInst{
            .instanceCustomIndex                    = (uint32_t)m_instTemplate.size(),
            .mask                                   = 0xFF,
            .instanceShaderBindingTableRecordOffset = 0,
            .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
            .accelerationStructureReference = m_fpGetASAddress(m_device, &addrInfo),
        };
        // glm column-major → VkTransformMatrixKHR row-major 3x4
        const glm::mat4& m = inst.model;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                vkInst.transform.matrix[r][c] = m[c][r];
        m_instTemplate.push_back(vkInst);

        VkBufferDeviceAddressInfo vboAddr{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = inst.vbo};
        VkBufferDeviceAddressInfo iboAddr{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = inst.ibo};
        geoTable.push_back({vkGetBufferDeviceAddress(m_device, &vboAddr),
                            vkGetBufferDeviceAddress(m_device, &iboAddr)});
    }
    m_instCount = (uint32_t)m_instTemplate.size();

    // Кэш трансформов для dirty-check: слоты собираются с этими значениями ниже,
    // поэтому стартовая версия совпадает (0) → нулевых пересборок в покое.
    m_lastModels.clear();
    m_lastModels.reserve(instances.size());
    for (const auto& inst : instances) m_lastModels.push_back(inst.model);
    m_transformVersion = 0;

    const VkDeviceSize instSize = m_instCount * sizeof(VkAccelerationStructureInstanceKHR);

    // Размер TLAS/scratch одинаков для всех кадров (одинаковое число инстансов)
    VkAccelerationStructureGeometryKHR tlasGeomSize{
        .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry     = {.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR}},
    };
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildSize{
        .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries   = &tlasGeomSize,
    };
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_fpGetASBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                        &tlasBuildSize, &m_instCount, &tlasSizes);

    // ── По TLAS на кадр-в-полёте: AS + инстанс-буфер (host-visible) + scratch ──
    m_frames.resize(m_frameCount);
    for (uint32_t i = 0; i < m_frameCount; ++i) {
        FrameTlas& F = m_frames[i];

        F.instanceBuffer = GpuBuffer::createHostVisible(
            m_phys, m_device, instSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        F.instMapped = F.instanceBuffer.map(m_device);
        std::memcpy(F.instMapped, m_instTemplate.data(), instSize);

        F.buffer = GpuBuffer::createDeviceLocalEmpty(
            m_phys, m_device, tlasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        VkAccelerationStructureCreateInfoKHR tlasCI{
            .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = F.buffer.handle,
            .size   = tlasSizes.accelerationStructureSize,
            .type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        if (m_fpCreateAS(m_device, &tlasCI, nullptr, &F.tlas) != VK_SUCCESS)
            throw std::runtime_error("vkCreateAccelerationStructureKHR (TLAS) failed");

        F.scratch = GpuBuffer::createDeviceLocalEmpty(
            m_phys, m_device, tlasSizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        recordTlasBuild(cmd, i);
    }

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    for (auto& s : scratchBuffers) s.destroy(m_device);
    vkFreeCommandBuffers(m_device, pool, 1, &cmd);

    // Таблица геометрии (host-visible SSBO) — статична, живёт до конца
    m_geoTableSize = geoTable.size() * sizeof(GeoEntry);
    m_geoTableBuffer = GpuBuffer::createHostVisible(
        m_phys, m_device, m_geoTableSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    void* geoMapped = m_geoTableBuffer.map(m_device);
    std::memcpy(geoMapped, geoTable.data(), m_geoTableSize);
    m_geoTableBuffer.unmap(m_device);

    Logger::info("RT: {} BLAS + {} TLAS ({} instances) built",
                 m_blas.size(), m_frameCount, m_instCount);
}

// ─── updateInstances ──────────────────────────────────────────────────────────
// Патчит трансформы шаблона, заливает в инстанс-буфер кадра, перестраивает его
// TLAS в cmd + барьер AS-build → чтение из шейдера (fragment / ray tracing).
void RtSystem::updateInstances(VkCommandBuffer cmd, uint32_t frame,
                               const std::vector<glm::mat4>& models) {
    if (!ready() || models.size() != m_instCount) return;

    // Патчим шаблон и поднимаем версию только при реальном изменении трансформов.
    if (m_lastModels != models) {
        for (uint32_t i = 0; i < m_instCount; ++i) {
            const glm::mat4& m = models[i];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    m_instTemplate[i].transform.matrix[r][c] = m[c][r];
        }
        m_lastModels = models;
        ++m_transformVersion;
    }

    // TLAS слота уже отражает текущую версию (двойная буферизация: каждый слот
    // догоняет за ≤ kMaxFrames кадров) → ни memcpy, ни пересборки, ни барьера.
    FrameTlas& F = m_frames[frame];
    if (F.version == m_transformVersion) return;
    F.version = m_transformVersion;

    std::memcpy(F.instMapped, m_instTemplate.data(),
                m_instCount * sizeof(VkAccelerationStructureInstanceKHR));

    recordTlasBuild(cmd, frame);

    // TLAS write → ray query read (scene_rt.frag / path tracer)
    VkMemoryBarrier2 barrier{
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    };
    VkDependencyInfo dep{
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace wsim
