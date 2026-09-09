#include "renderer/Mesh.hpp"

#include <stdexcept>

namespace wsim {

Mesh Mesh::create(
    VkPhysicalDevice             physDevice,
    VkDevice                     device,
    VkCommandPool                cmdPool,
    VkQueue                      queue,
    const std::vector<Vertex>&   vertices,
    const std::vector<uint32_t>& indices,
    VkBufferUsageFlags           extraUsage
) {
    if (vertices.empty() || indices.empty())
        throw std::runtime_error("Mesh::create: пустые данные вершин/индексов");

    Mesh mesh;
    mesh.indexCount  = static_cast<uint32_t>(indices.size());
    mesh.vertexCount = static_cast<uint32_t>(vertices.size());

    // Вычислить локальный AABB
    for (const auto& v : vertices)
        mesh.localAABB.expand(glm::vec3(v.position));

    // Загрузить на GPU: оба буфера одним submit
    UploadBatch batch(physDevice, device, cmdPool, queue);
    mesh.vbo = batch.add(
        vertices.data(), vertices.size() * sizeof(Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | extraUsage
    );
    mesh.ibo = batch.add(
        indices.data(), indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | extraUsage
    );
    batch.submit();

    return mesh;
}

void Mesh::destroy(VkDevice device) {
    vbo.destroy(device);
    ibo.destroy(device);
    indexCount = 0;
    localAABB  = AABB{};
}

}  // namespace wsim
