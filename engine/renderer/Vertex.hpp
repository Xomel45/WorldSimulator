#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>

namespace wsim {

struct Vertex {
    glm::vec4 position;  // xyz = позиция, w = исходная Y (baseline для deform)
    glm::vec4 normal;    // xyz = нормаль, w = зарезервировано
    glm::vec4 color;     // rgb = albedo (MTL Kd или процедурный), a = 1

    [[nodiscard]] static VkVertexInputBindingDescription bindingDescription() noexcept {
        return {
            .binding   = 0,
            .stride    = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
    }

    [[nodiscard]] static std::array<VkVertexInputAttributeDescription, 3>
    attributeDescriptions() noexcept {
        return {{
            // location 0: position (vec4)
            {
                .location = 0,
                .binding  = 0,
                .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                .offset   = offsetof(Vertex, position),
            },
            // location 1: normal (vec4)
            {
                .location = 1,
                .binding  = 0,
                .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                .offset   = offsetof(Vertex, normal),
            },
            // location 2: color (vec4)
            {
                .location = 2,
                .binding  = 0,
                .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                .offset   = offsetof(Vertex, color),
            },
        }};
    }
};

}  // namespace wsim
