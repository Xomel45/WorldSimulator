#include "renderer/PathTracer.hpp"

#include "core/Logger.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace wsim {

namespace {
constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
}

// ─── loadShader ───────────────────────────────────────────────────────────────
VkShaderModule PathTracer::loadShader(const std::string& path) const {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("PathTracer: не открыть шейдер " + path);
    const size_t size = (size_t)f.tellg();
    std::vector<char> code(size);
    f.seekg(0);
    f.read(code.data(), (std::streamsize)size);

    VkShaderModuleCreateInfo ci{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = reinterpret_cast<const uint32_t*>(code.data()),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: vkCreateShaderModule " + path);
    return module;
}

// ─── init ─────────────────────────────────────────────────────────────────────
void PathTracer::init(VkDevice device, VkPhysicalDevice phys, uint32_t frameCount,
                      const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& props,
                      const std::string& shaderDir) {
    m_device = device;
    m_phys   = phys;
    m_frames = frameCount;
    m_props  = props;

    const auto load = [&](const char* n) { return vkGetDeviceProcAddr(m_device, n); };
    m_fpCreatePipelines = (PFN_vkCreateRayTracingPipelinesKHR)load("vkCreateRayTracingPipelinesKHR");
    m_fpGetHandles      = (PFN_vkGetRayTracingShaderGroupHandlesKHR)load("vkGetRayTracingShaderGroupHandlesKHR");
    m_fpCmdTraceRays    = (PFN_vkCmdTraceRaysKHR)load("vkCmdTraceRaysKHR");

    // ── Descriptor set layout ─────────────────────────────────────────────────
    // 0 camera UBO, 1 lights UBO, 2 TLAS, 3 output storage image, 4 geo-table SSBO
    const VkShaderStageFlags rgen = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    const VkShaderStageFlags chit = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    VkDescriptorSetLayoutBinding binds[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1, .stageFlags = rgen | chit},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1, .stageFlags = chit},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
         .descriptorCount = 1, .stageFlags = rgen | chit},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1, .stageFlags = rgen},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = chit},
    };
    VkDescriptorSetLayoutCreateInfo layoutCI{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = binds,
    };
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_descLayout) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: descriptor set layout");

    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         2 * m_frames},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, m_frames},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          m_frames},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         m_frames},
    };
    VkDescriptorPoolCreateInfo poolCI{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = m_frames, .poolSizeCount = 4, .pPoolSizes = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: descriptor pool");

    std::vector<VkDescriptorSetLayout> layouts(m_frames, m_descLayout);
    m_descSets.resize(m_frames);
    VkDescriptorSetAllocateInfo allocI{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descPool, .descriptorSetCount = m_frames,
        .pSetLayouts = layouts.data(),
    };
    if (vkAllocateDescriptorSets(m_device, &allocI, m_descSets.data()) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: allocate descriptor sets");

    createPipeline(shaderDir);
    createSbt();
    Logger::info("PathTracer ready (RT pipeline + SBT)");
}

// ─── createPipeline ───────────────────────────────────────────────────────────
void PathTracer::createPipeline(const std::string& shaderDir) {
    VkShaderModule rgen  = loadShader(shaderDir + "pathtrace.rgen.spv");
    VkShaderModule rmiss = loadShader(shaderDir + "pathtrace.rmiss.spv");
    VkShaderModule rchit = loadShader(shaderDir + "pathtrace.rchit.spv");

    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,      .module = rgen,  .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_MISS_BIT_KHR,        .module = rmiss, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, .module = rchit, .pName = "main"},
    };

    VkRayTracingShaderGroupCreateInfoKHR groups[] = {
        {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
         .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
         .generalShader = 0, .closestHitShader = VK_SHADER_UNUSED_KHR,
         .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR},
        {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
         .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
         .generalShader = 1, .closestHitShader = VK_SHADER_UNUSED_KHR,
         .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR},
        {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
         .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
         .generalShader = VK_SHADER_UNUSED_KHR, .closestHitShader = 2,
         .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR},
    };

    VkPushConstantRange pcRange{
        .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        .offset = 0, .size = sizeof(PushConstants),
    };
    VkPipelineLayoutCreateInfo plCI{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &m_descLayout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcRange,
    };
    if (vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: pipeline layout");

    VkRayTracingPipelineCreateInfoKHR ci{
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .stageCount = 3, .pStages = stages,
        .groupCount = 3, .pGroups = groups,
        .maxPipelineRayRecursionDepth = 1,  // rgen→chit; тени в chit — через ray_query
        .layout = m_pipelineLayout,
    };
    if (m_fpCreatePipelines(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci,
                            nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: vkCreateRayTracingPipelinesKHR");

    vkDestroyShaderModule(m_device, rgen, nullptr);
    vkDestroyShaderModule(m_device, rmiss, nullptr);
    vkDestroyShaderModule(m_device, rchit, nullptr);
}

// ─── createSbt ────────────────────────────────────────────────────────────────
void PathTracer::createSbt() {
    const uint32_t handleSize   = m_props.shaderGroupHandleSize;
    const uint32_t handleStride = alignUp(handleSize, m_props.shaderGroupHandleAlignment);
    const uint32_t base         = m_props.shaderGroupBaseAlignment;

    const VkDeviceSize rgenOffset = 0;
    const VkDeviceSize missOffset = alignUp(handleStride, base);
    const VkDeviceSize hitOffset  = alignUp((uint32_t)missOffset + handleStride, base);
    const VkDeviceSize sbtSize    = hitOffset + handleStride;

    std::vector<uint8_t> handles(3 * handleSize);
    if (m_fpGetHandles(m_device, m_pipeline, 0, 3, handles.size(), handles.data()) != VK_SUCCESS)
        throw std::runtime_error("PathTracer: vkGetRayTracingShaderGroupHandlesKHR");

    m_sbt = GpuBuffer::createHostVisible(
        m_phys, m_device, sbtSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    auto* p = static_cast<uint8_t*>(m_sbt.map(m_device));
    std::memcpy(p + rgenOffset, handles.data() + 0 * handleSize, handleSize);
    std::memcpy(p + missOffset, handles.data() + 1 * handleSize, handleSize);
    std::memcpy(p + hitOffset,  handles.data() + 2 * handleSize, handleSize);
    m_sbt.unmap(m_device);

    const VkDeviceAddress addr = m_sbt.address(m_device);
    m_rgenRegion = {addr + rgenOffset, handleStride, handleStride};
    m_missRegion = {addr + missOffset, handleStride, handleStride};
    m_hitRegion  = {addr + hitOffset,  handleStride, handleStride};
    m_callRegion = {0, 0, 0};
}

// ─── configure ────────────────────────────────────────────────────────────────
void PathTracer::configure(uint32_t f, VkAccelerationStructureKHR tlas,
                           VkBuffer cameraUBO, VkBuffer lightsUBO,
                           VkBuffer geoTable, VkDeviceSize geoSize,
                           VkImageView outputView) {
    VkDescriptorBufferInfo camI{cameraUBO, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo litI{lightsUBO, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo geoI{geoTable,  0, geoSize};
    VkDescriptorImageInfo  outI{.imageView = outputView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSetAccelerationStructureKHR asW{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1, .pAccelerationStructures = &tlas,
    };
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_descSets[f],
         .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &camI},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_descSets[f],
         .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &litI},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &asW, .dstSet = m_descSets[f],
         .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_descSets[f],
         .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &outI},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_descSets[f],
         .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &geoI},
    };
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
}

// ─── trace ────────────────────────────────────────────────────────────────────
void PathTracer::trace(VkCommandBuffer cmd, uint32_t f, VkExtent2D extent,
                       const glm::mat4& invViewProj, uint32_t frameIndex, uint32_t spp) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descSets[f], 0, nullptr);
    PushConstants pc{invViewProj, frameIndex, spp};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(pc), &pc);
    m_fpCmdTraceRays(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                     extent.width, extent.height, 1);
}

// ─── destroy ──────────────────────────────────────────────────────────────────
void PathTracer::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    m_sbt.destroy(m_device);
    if (m_pipeline)       vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descPool)       vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    if (m_descLayout)     vkDestroyDescriptorSetLayout(m_device, m_descLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
}

}  // namespace wsim
