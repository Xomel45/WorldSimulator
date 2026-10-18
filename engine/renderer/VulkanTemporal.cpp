// Temporal-апскейлер и генерация кадров -- часть VulkanContext (общие ресурсы, барьеры, swapchain),
// вынесена в отдельный файл, чтобы VulkanContext.cpp не рос дальше.
//
// Кадр с Upscaler::Temporal:
//   1. updateCamera: prevViewProj + субпиксельный джиттер (последовательность Halton 2,3);
//   2. сцена пишет цвет, расстояние и вектор движения (scene.vert/frag);
//   3. taau.comp: цвет + движение + история → новая история (полное разрешение);
//   4. RCAS (из FSR 1.0): история → m_upscaled (резкость), его же читает composite.
// Генерация кадров (поверх Temporal, нужна вертикальная синхронизация):
//   5. framegen.comp: прошлый показанный кадр + текущий + движение → промежуточный кадр;
//      показываем промежуточный, затем настоящий (endFrame), интерфейс рисуется на обоих.

#include "renderer/VulkanContext.hpp"

#include "core/Logger.hpp"
#include "renderer/FsrCon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace wsim {

namespace {

float halton(uint32_t index, uint32_t base) {
    float f = 1.f, r = 0.f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

// Device-local 2D-образ + view (все образы temporal-цепочки одного вида).
void createImage2D(VkDevice dev, VkPhysicalDevice phys, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                   VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ci{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = {extent.width, extent.height, 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ci, nullptr, &img) != VK_SUCCESS) throw std::runtime_error("vkCreateImage (temporal) failed");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) throw std::runtime_error("Нет памяти для temporal-образа");
    VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (vkAllocateMemory(dev, &alloc, nullptr, &mem) != VK_SUCCESS) throw std::runtime_error("vkAllocateMemory (temporal) failed");
    vkBindImageMemory(dev, img, mem, 0);
    VkImageViewCreateInfo vi{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = img,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) throw std::runtime_error("vkCreateImageView (temporal) failed");
}

void destroyImage2D(VkDevice dev, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    if (img == VK_NULL_HANDLE) return;
    vkDestroyImageView(dev, view, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyImage(dev, img, nullptr);
    img = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
}

VkDescriptorSetLayoutBinding binding(uint32_t index, VkDescriptorType type) {
    return {.binding = index, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

}  // namespace

// ─── Камера: история и джиттер ────────────────────────────────────────────────

void VulkanContext::advanceJitter() {
    // Фаз столько, чтобы за их период пиксель вывода накрыли сэмплами: ~8 при масштабе 1, больше при сильном апскейле.
    const float inv    = 1.0f / std::max(m_renderScale, 0.25f);
    const auto  phases = static_cast<uint32_t>(std::clamp(std::ceil(8.0f * inv * inv), 8.0f, 32.0f));
    m_jitterIndex      = (m_jitterIndex + 1) % phases;
    m_jitterPx         = {halton(m_jitterIndex + 1, 2) - 0.5f, halton(m_jitterIndex + 1, 3) - 0.5f};
}

void VulkanContext::updateCameraHistory(CameraData& cam) {
    const glm::mat4 vp = cam.proj * cam.view;
    cam.prevViewProj   = m_havePrevViewProj ? m_prevViewProj : vp;
    m_prevViewProj     = vp;
    m_havePrevViewProj = true;

    const bool temporal = m_upscaler == Upscaler::Temporal && !(m_pathTraced && m_pt.ready());
    if (temporal) {
        advanceJitter();
        cam.jitter = glm::vec4(2.f * m_jitterPx.x / static_cast<float>(m_renderExtent.width),
                               2.f * m_jitterPx.y / static_cast<float>(m_renderExtent.height), 0.f, 0.f);
    } else {
        m_jitterPx = {0.f, 0.f};
        cam.jitter = glm::vec4(0.f);
    }
}

// ─── Пайплайны ────────────────────────────────────────────────────────────────

void VulkanContext::createTemporalPipelines() {
    VkSamplerCreateInfo sci{
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(m_device, &sci, nullptr, &m_temporalSampler) != VK_SUCCESS) throw std::runtime_error("vkCreateSampler (temporal) failed");

    auto makeLayout = [&](const VkDescriptorSetLayoutBinding* b, uint32_t n, uint32_t pushSize, VkDescriptorSetLayout& dsl, VkPipelineLayout& pl) {
        VkDescriptorSetLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &dsl) != VK_SUCCESS) throw std::runtime_error("vkCreateDescriptorSetLayout (temporal) failed");
        VkPushConstantRange pc{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = pushSize};
        VkPipelineLayoutCreateInfo pi{
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &dsl,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pc,
        };
        if (vkCreatePipelineLayout(m_device, &pi, nullptr, &pl) != VK_SUCCESS) throw std::runtime_error("vkCreatePipelineLayout (temporal) failed");
    };
    auto makeCompute = [&](const char* file, VkPipelineLayout layout, VkPipeline& pipe) {
        VkShaderModule mod = loadShader(m_shaderDir + file);
        VkComputePipelineCreateInfo ci{
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"},
            .layout = layout,
        };
        if (vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) throw std::runtime_error("vkCreateComputePipelines (temporal) failed");
        vkDestroyShaderModule(m_device, mod, nullptr);
    };

    const VkDescriptorSetLayoutBinding taau[] = {
        binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        binding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
    };
    makeLayout(taau, 5, 32, m_taauDescLayout, m_taauPipeLayout);
    makeCompute("taau.comp.spv", m_taauPipeLayout, m_taauPipeline);

    const VkDescriptorSetLayoutBinding fg[] = {
        binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
    };
    makeLayout(fg, 4, 16, m_fgDescLayout, m_fgPipeLayout);
    makeCompute("framegen.comp.spv", m_fgPipeLayout, m_fgPipeline);

    // Наборы: 2 taau + 2 rcas(история) + 1 fg + 1 composite(промежуточный кадр)
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 4 + 2 + 3 + 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 + 2 + 1},
    };
    VkDescriptorPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 6, .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_temporalPool) != VK_SUCCESS) throw std::runtime_error("vkCreateDescriptorPool (temporal) failed");

    VkDescriptorSetLayout layouts[6] = {m_taauDescLayout, m_taauDescLayout, m_fsrDescSetLayout, m_fsrDescSetLayout, m_fgDescLayout, m_blurDescSetLayout};
    VkDescriptorSet       sets[6]{};
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_temporalPool, .descriptorSetCount = 6, .pSetLayouts = layouts};
    if (vkAllocateDescriptorSets(m_device, &ai, sets) != VK_SUCCESS) throw std::runtime_error("vkAllocateDescriptorSets (temporal) failed");
    m_dsTaau[0] = sets[0];
    m_dsTaau[1] = sets[1];
    m_dsRcasHist[0] = sets[2];
    m_dsRcasHist[1] = sets[3];
    m_dsFg = sets[4];
    m_dsCompositeInterp = sets[5];
    Logger::info("Temporal upscaler ready (TAAU + frame generation)");
}

void VulkanContext::destroyTemporalPipelines() {
    vkDestroyPipeline(m_device, m_fgPipeline, nullptr);
    vkDestroyPipeline(m_device, m_taauPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_fgPipeLayout, nullptr);
    vkDestroyPipelineLayout(m_device, m_taauPipeLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_temporalPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_fgDescLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_taauDescLayout, nullptr);
    vkDestroySampler(m_device, m_temporalSampler, nullptr);
}

// ─── Таргеты ──────────────────────────────────────────────────────────────────

void VulkanContext::createTemporalTargets() {
    m_historyValid   = false;
    m_prevFrameValid = false;
    m_historyCur     = 0;
    if (m_upscaler != Upscaler::Temporal) return;

    constexpr VkImageUsageFlags kHist = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 2; ++i) {
        createImage2D(m_device, m_physDevice, m_scExtent, kHistoryFormat, kHist, m_historyImage[i], m_historyMemory[i], m_historyView[i]);
        m_historyLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    // Прошлый кадр и промежуточный нужны только при генерации кадров
    if (m_frameGenWanted) {
        createImage2D(m_device, m_physDevice, m_scExtent, kOffscreenFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      m_prevImage, m_prevMemory, m_prevView);
        createImage2D(m_device, m_physDevice, m_scExtent, kOffscreenFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      m_interpImage, m_interpMemory, m_interpView);
        m_prevLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
        m_interpLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    writeTemporalDescriptors();
}

void VulkanContext::destroyTemporalTargets() {
    for (int i = 0; i < 2; ++i) destroyImage2D(m_device, m_historyImage[i], m_historyMemory[i], m_historyView[i]);
    destroyImage2D(m_device, m_prevImage, m_prevMemory, m_prevView);
    destroyImage2D(m_device, m_interpImage, m_interpMemory, m_interpView);
}

void VulkanContext::writeTemporalDescriptors() {
    auto sampled = [&](VkImageView v) { return VkDescriptorImageInfo{m_temporalSampler, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; };
    auto storage = [&](VkImageView v) { return VkDescriptorImageInfo{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL}; };
    auto write = [&](VkDescriptorSet ds, uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b, .descriptorCount = 1, .descriptorType = t, .pImageInfo = info};
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    };
    constexpr auto kSampler = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr auto kStorage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    const VkDescriptorImageInfo color = sampled(m_offscreenView), dist = sampled(m_viewDistResolveView), motion = sampled(m_motionResolveView);
    for (uint32_t cur = 0; cur < 2; ++cur) {
        const VkDescriptorImageInfo hist = sampled(m_historyView[1 - cur]), out = storage(m_historyView[cur]);
        write(m_dsTaau[cur], 0, kSampler, &color);
        write(m_dsTaau[cur], 1, kSampler, &dist);
        write(m_dsTaau[cur], 2, kSampler, &motion);
        write(m_dsTaau[cur], 3, kSampler, &hist);
        write(m_dsTaau[cur], 4, kStorage, &out);
        // RCAS: история[cur] → upscaled
        const VkDescriptorImageInfo rin = sampled(m_historyView[cur]), rout = storage(m_upscaledView);
        write(m_dsRcasHist[cur], 0, kSampler, &rin);
        write(m_dsRcasHist[cur], 1, kStorage, &rout);
    }
    if (m_interpImage != VK_NULL_HANDLE) {
        const VkDescriptorImageInfo prev = sampled(m_prevView), cur = sampled(m_upscaledView), out = storage(m_interpView), interp = sampled(m_interpView);
        write(m_dsFg, 0, kSampler, &prev);
        write(m_dsFg, 1, kSampler, &cur);
        write(m_dsFg, 2, kSampler, &motion);
        write(m_dsFg, 3, kStorage, &out);
        write(m_dsCompositeInterp, 0, kSampler, &interp);
    }
}

// ─── Кадр ─────────────────────────────────────────────────────────────────────

void VulkanContext::recordTemporalUpscale(VkCommandBuffer cmd) {
    constexpr auto kRead    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr auto kGeneral = VK_IMAGE_LAYOUT_GENERAL;

    // Расстояние и движение: в SHADER_READ (их мог перевести vision-проход, а мог и нет)
    if (m_viewDistResolveLayout != kRead) {
        transitionImage(cmd, m_viewDistResolveImage, m_viewDistResolveLayout, kRead);
        m_viewDistResolveLayout = kRead;
    }
    if (m_motionResolveLayout != kRead) {
        transitionImage(cmd, m_motionResolveImage, m_motionResolveLayout, kRead);
        m_motionResolveLayout = kRead;
    }

    const uint32_t cur = m_historyCur, prev = 1 - cur;
    if (m_historyLayout[prev] != kRead) {  // ещё ни разу не писали: содержимое не используется (reset), нужен только layout
        transitionImage(cmd, m_historyImage[prev], m_historyLayout[prev], kRead);
        m_historyLayout[prev] = kRead;
    }
    transitionImage(cmd, m_historyImage[cur], m_historyLayout[cur], kGeneral);
    m_historyLayout[cur] = kGeneral;

    struct PC {
        float renderW, renderH, outW, outH, jx, jy, reset, pad;
    } pc{static_cast<float>(m_renderExtent.width), static_cast<float>(m_renderExtent.height),
         static_cast<float>(m_scExtent.width),    static_cast<float>(m_scExtent.height),
         m_jitterPx.x, m_jitterPx.y, m_historyValid ? 0.f : 1.f, 0.f};
    // push-константы шейдера: renderSize, outSize, jitter, reset, pad
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_taauPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_taauPipeLayout, 0, 1, &m_dsTaau[cur], 0, nullptr);
    vkCmdPushConstants(cmd, m_taauPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (m_scExtent.width + 7) / 8, (m_scExtent.height + 7) / 8, 1);

    // история[cur] → читаемая; RCAS пишет в upscaled
    transitionImage(cmd, m_historyImage[cur], kGeneral, kRead);
    m_historyLayout[cur] = kRead;
    transitionImage(cmd, m_upscaledImage, m_upscaledLayout, kGeneral);
    m_upscaledLayout = kGeneral;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_rcasPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fsrPipelineLayout, 0, 1, &m_dsRcasHist[cur], 0, nullptr);
    vkCmdPushConstants(cmd, m_fsrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_rcasCon), m_rcasCon);
    vkCmdDispatch(cmd, (m_scExtent.width + 15) / 16, (m_scExtent.height + 15) / 16, 1);

    transitionImage(cmd, m_upscaledImage, m_upscaledLayout, kRead);
    m_upscaledLayout = kRead;

    m_historyCur   = prev;  // в следующем кадре читаем то, что записали сейчас
    m_historyValid = true;
}

void VulkanContext::recordFrameGeneration(VkCommandBuffer cmd) {
    constexpr auto kRead    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr auto kGeneral = VK_IMAGE_LAYOUT_GENERAL;
    if (m_prevLayout != kRead) {
        transitionImage(cmd, m_prevImage, m_prevLayout, kRead);
        m_prevLayout = kRead;
    }
    transitionImage(cmd, m_interpImage, m_interpLayout, kGeneral);
    m_interpLayout = kGeneral;

    const float pc[4] = {static_cast<float>(m_scExtent.width), static_cast<float>(m_scExtent.height), 0.f, 0.f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fgPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fgPipeLayout, 0, 1, &m_dsFg, 0, nullptr);
    vkCmdPushConstants(cmd, m_fgPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, (m_scExtent.width + 7) / 8, (m_scExtent.height + 7) / 8, 1);

    transitionImage(cmd, m_interpImage, kGeneral, kRead);
    m_interpLayout = kRead;
}

// Копия показанного (настоящего) кадра -- «прошлый кадр» для интерполяции в следующий раз.
void VulkanContext::recordStorePrevFrame(VkCommandBuffer cmd) {
    constexpr auto kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transitionImage(cmd, m_upscaledImage, m_upscaledLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImage(cmd, m_prevImage, m_prevLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent         = {m_scExtent.width, m_scExtent.height, 1},
    };
    vkCmdCopyImage(cmd, m_upscaledImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_prevImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(cmd, m_upscaledImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kRead);
    transitionImage(cmd, m_prevImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRead);
    m_upscaledLayout = kRead;
    m_prevLayout     = kRead;
    m_prevFrameValid = true;
}

// Composite на образ swapchain: проход остаётся открытым (дальше интерфейс).
void VulkanContext::recordPresentPass(VkCommandBuffer cmd, uint32_t imageIdx, VkDescriptorSet source, bool drawUi) {
    transitionImage(cmd, m_scImages[imageIdx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_scImageViews[imageIdx],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, m_scExtent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color,
    };
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, static_cast<float>(m_scExtent.width), static_cast<float>(m_scExtent.height), 0.f, 1.f};
    const VkRect2D   sc{{0, 0}, m_scExtent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipelineLayout, 0, 1, &source, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (drawUi && m_uiRecorder) m_uiRecorder(cmd);
}

}  // namespace wsim
