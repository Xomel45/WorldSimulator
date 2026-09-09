#pragma once

#include "renderer/GpuBuffer.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wsim {

// Полноценный RT-пайплайн (VK_KHR_ray_tracing_pipeline): raygen/miss/closesthit
// + SBT для path-traced режима. TLAS/geo-table берутся из RtSystem, camera/lights
// UBO и offscreen-таргет — из VulkanContext (передаются в configure/trace).
class PathTracer {
public:
    struct PushConstants {
        glm::mat4 invViewProj;
        uint32_t  frameIndex;  // decorrelation RNG между кадрами
        uint32_t  spp;         // сэмплов за кадр
    };

    // Создаёт пайплайн + SBT. props нужны для выравнивания SBT.
    void init(VkDevice device, VkPhysicalDevice phys, uint32_t frameCount,
              const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& props,
              const std::string& shaderDir);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

    // Привязать входы кадра f. Вызывать после build AS и после каждого recreate
    // (меняется outputView) — для каждого frame-in-flight.
    void configure(uint32_t f, VkAccelerationStructureKHR tlas,
                   VkBuffer cameraUBO, VkBuffer lightsUBO,
                   VkBuffer geoTable, VkDeviceSize geoSize,
                   VkImageView outputView);

    // Трассировка кадра. outputImage должен быть в GENERAL до вызова.
    void trace(VkCommandBuffer cmd, uint32_t f, VkExtent2D extent,
               const glm::mat4& invViewProj, uint32_t frameIndex, uint32_t spp);

private:
    [[nodiscard]] VkShaderModule loadShader(const std::string& path) const;
    void createPipeline(const std::string& shaderDir);
    void createSbt();

    VkDevice         m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys   = VK_NULL_HANDLE;
    uint32_t         m_frames = 0;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_props{};

    VkDescriptorSetLayout        m_descLayout     = VK_NULL_HANDLE;
    VkDescriptorPool             m_descPool       = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets;
    VkPipelineLayout             m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline                   m_pipeline       = VK_NULL_HANDLE;

    GpuBuffer                       m_sbt;
    VkStridedDeviceAddressRegionKHR m_rgenRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitRegion{};
    VkStridedDeviceAddressRegionKHR m_callRegion{};

    PFN_vkCreateRayTracingPipelinesKHR       m_fpCreatePipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR m_fpGetHandles      = nullptr;
    PFN_vkCmdTraceRaysKHR                    m_fpCmdTraceRays    = nullptr;
};

}  // namespace wsim
