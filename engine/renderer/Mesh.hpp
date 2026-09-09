#pragma once

#include "renderer/AABB.hpp"
#include "renderer/GpuBuffer.hpp"
#include "renderer/Vertex.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace wsim {

// GPU-меш: вершинный буфер + индексный буфер + локальный AABB.
// Создаётся один раз; несколько SceneObject могут ссылаться на один Mesh.
struct Mesh {
    GpuBuffer vbo;
    GpuBuffer ibo;
    uint32_t  indexCount  = 0;
    uint32_t  vertexCount = 0;
    AABB      localAABB{};   // в локальном (mesh) пространстве

    // Создать меш на GPU из CPU-данных.
    // AABB вычисляется автоматически из вершин.
    // extraUsage: доп. флаги для vbo/ibo (VulkanContext::meshBufferUsage() для RT).
    [[nodiscard]] static Mesh create(
        VkPhysicalDevice            physDevice,
        VkDevice                    device,
        VkCommandPool               cmdPool,
        VkQueue                     queue,
        const std::vector<Vertex>&  vertices,
        const std::vector<uint32_t>& indices,
        VkBufferUsageFlags          extraUsage = 0
    );

    void destroy(VkDevice device);
};

}  // namespace wsim
