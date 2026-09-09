#include "renderer/VulkanContext.hpp"

#include "core/Logger.hpp"
#include "renderer/FsrCon.hpp"
#include "renderer/Vertex.hpp"
#include "renderer/Window.hpp"

// Официальный SDK NVIDIA Image Scaling (NVIDIAGameWorks, MIT):
// NISConfig + NVScalerUpdateConfig + коэффициенты фильтров (coef_scale/coef_usm)
#include "renderer/nis/NIS_Config.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace wsim {

// ─── Compile-time constants ───────────────────────────────────────────────────
#ifdef NDEBUG
static constexpr bool kValidation = false;
#else
static constexpr bool kValidation = true;
#endif

static const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
};
static const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// RT-расширения — опциональны: включаются при наличии, иначе fallback на растеризацию
static const std::vector<const char*> kRayQueryExtensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,  // требуется accel structure
};

// Полноценный RT-пайплайн (raygen/miss/closesthit + SBT) — поверх ray_query,
// нужен для path-traced режима. Опционален; включается при наличии.
static const char* kRayTracingPipelineExtension = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;

// ─── Local helpers ─────────────────────────────────────────────────────────────
namespace {

bool checkValidationLayers() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());

    for (const char* name : kValidationLayers) {
        bool found = false;
        for (const auto& p : props)
            if (std::strcmp(name, p.layerName) == 0) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return formats[0];
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (!vsync) {
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR)   return m;
        for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t w, uint32_t h) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
        return caps.currentExtent;
    return {
        std::clamp(w, caps.minImageExtent.width,  caps.maxImageExtent.width),
        std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height),
    };
}

}  // namespace

// ─── Constructor ──────────────────────────────────────────────────────────────
VulkanContext::VulkanContext(Window& window, bool vsync, std::string shaderDir)
    : m_window(&window), m_vsync(vsync), m_shaderDir(std::move(shaderDir)) {
    createInstance();
    if (kValidation) setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice();
    createPipelineCache();
    createSwapchain(window.width(), window.height());
    createImageViews();
    createDepthResources();
    createOffscreenResources();
    createBlurPipeline();
    createVisionPipeline();
    createSyncObjects();
    createCommandPool();
    createCommandBuffers();
    createCameraUBOs();
    createDescriptors();
    createScenePipeline();
    createUpscalePipelines();
    createUpscaleTargets();
    if (m_rtPipeline)
        m_pt.init(m_device, m_physDevice, kMaxFrames, m_rtPipeProps, m_shaderDir);
    Logger::info("VulkanContext ready");
}

// ─── Destructor ───────────────────────────────────────────────────────────────
VulkanContext::~VulkanContext() {
    vkDeviceWaitIdle(m_device);

    // Pipeline cache: сохранить на диск, затем уничтожить
    savePipelineCache();
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);

    // Upscalers
    destroyUpscaleTargets();
    vkDestroyPipeline(m_device, m_nisPipeline, nullptr);
    vkDestroyPipeline(m_device, m_rcasPipeline, nullptr);
    vkDestroyPipeline(m_device, m_easuPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_nisPipelineLayout, nullptr);
    vkDestroyPipelineLayout(m_device, m_fsrPipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_upscaleDescPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_nisDescSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_fsrDescSetLayout, nullptr);
    vkDestroySampler(m_device, m_nisSampler, nullptr);
    m_nisConfigUBO.destroy(m_device);
    vkDestroyImageView(m_device, m_nisCoefScaleView, nullptr);
    vkFreeMemory(m_device, m_nisCoefScaleMemory, nullptr);
    vkDestroyImage(m_device, m_nisCoefScaleImage, nullptr);
    vkDestroyImageView(m_device, m_nisCoefUsmView, nullptr);
    vkFreeMemory(m_device, m_nisCoefUsmMemory, nullptr);
    vkDestroyImage(m_device, m_nisCoefUsmImage, nullptr);

    // Ray tracing (AS, geo-table, fps — всё внутри RtSystem)
    m_pt.destroy();
    m_rt.destroy();

    // Scene pipelines
    vkDestroyPipeline(m_device, m_scenePipelineRT, nullptr);
    vkDestroyPipeline(m_device, m_scenePipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr);

    // Vision defocus pipeline
    vkDestroyPipeline(m_device, m_visionPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_visionPipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_visionDescPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_visionDescSetLayout, nullptr);

    // Blur / composite pipeline
    vkDestroyPipeline(m_device, m_compositePipeline, nullptr);
    vkDestroyPipeline(m_device, m_blurPipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_blurPipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_blurDescPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_blurDescSetLayout, nullptr);
    vkDestroySampler(m_device, m_blurSampler, nullptr);

    // Offscreen + blur temp + MSAA images
    vkDestroyImageView(m_device, m_blurTempView, nullptr);
    vkFreeMemory(m_device, m_blurTempMemory, nullptr);
    vkDestroyImage(m_device, m_blurTempImage, nullptr);
    vkDestroyImageView(m_device, m_offscreenView, nullptr);
    vkFreeMemory(m_device, m_offscreenMemory, nullptr);
    vkDestroyImage(m_device, m_offscreenImage, nullptr);
    if (m_msaaColorImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_msaaColorView, nullptr);
        vkFreeMemory(m_device, m_msaaColorMemory, nullptr);
        vkDestroyImage(m_device, m_msaaColorImage, nullptr);
    }

    // View-distance targets
    vkDestroyImageView(m_device, m_viewDistResolveView, nullptr);
    vkFreeMemory(m_device, m_viewDistResolveMemory, nullptr);
    vkDestroyImage(m_device, m_viewDistResolveImage, nullptr);
    if (m_viewDistImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_viewDistView, nullptr);
        vkFreeMemory(m_device, m_viewDistMemory, nullptr);
        vkDestroyImage(m_device, m_viewDistImage, nullptr);
    }

    // Descriptors (camera)
    vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

    // Camera + Lights UBOs
    for (auto& buf : m_cameraUBOs) buf.destroy(m_device);
    for (auto& buf : m_lightsUBOs) buf.destroy(m_device);

    // Depth buffer
    vkDestroyImageView(m_device, m_depthView, nullptr);
    vkFreeMemory(m_device, m_depthMemory, nullptr);
    vkDestroyImage(m_device, m_depthImage, nullptr);

    vkDestroyCommandPool(m_device, m_cmdPool, nullptr);

    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
        vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
    }
    for (auto sem : m_renderFinished)
        vkDestroySemaphore(m_device, sem, nullptr);

    for (auto v : m_scImageViews) vkDestroyImageView(m_device, v, nullptr);
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    vkDestroyDevice(m_device, nullptr);

    if (kValidation && m_debugMessenger != VK_NULL_HANDLE)
        m_fpDestroyDebugMessenger(m_instance, m_debugMessenger, nullptr);

    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);

    Logger::info("VulkanContext destroyed");
}

// ─── Frame: beginFrame ────────────────────────────────────────────────────────
void VulkanContext::beginFrame() {
    m_frameValid = false;

    // 0. Отложенное пересоздание swapchain (resize окна, смена vsync, SUBOPTIMAL)
    if (m_window->consumeResized() || m_needsRecreate) {
        m_needsRecreate = false;
        recreateSwapchain();
    }

    // 1. Ждём, пока GPU закончит предыдущий кадр
    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    // 2. Запрашиваем следующий image
    VkResult result = vkAcquireNextImageKHR(
        m_device, m_swapchain, UINT64_MAX,
        m_imageAvailable[m_currentFrame], VK_NULL_HANDLE,
        &m_currentImageIdx
    );
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Семафор не сигналится при OUT_OF_DATE — безопасно пересоздать и пропустить кадр
        recreateSwapchain();
        return;
    }
    // VK_SUBOPTIMAL_KHR допустим — продолжаем

    // 3. Сбрасываем fence только если будем submit'ить
    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    // 4. Записываем command buffer
    auto cmd = m_cmdBuffers[m_currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Динамика: перестраиваем TLAS текущего кадра под актуальные трансформы
    // (физика двигает объекты). Иначе RT-тени/AO/GI/PT трассируются по позициям
    // спавна. Сборку AS делаем вне render pass — здесь, до scene/PT-пасса.
    if (m_rt.ready() && (m_rtShadows || m_pathTraced) && !m_dynamicTransforms.empty())
        m_rt.updateInstances(cmd, m_currentFrame, m_dynamicTransforms);

    // Path-traced режим: растровый scene pass не открываем — сцену трассируем
    // целиком в beginUIPass (после updateCamera). drawMesh становится no-op.
    if (m_pathTraced && m_pt.ready()) {
        m_frameValid = true;
        return;
    }

    // 5. Offscreen уже в COLOR_ATTACHMENT_OPTIMAL (endFrame возвращает его туда).
    //    Первый кадр: m_offscreenLayout == UNDEFINED — переходим один раз.
    if (m_offscreenLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        transitionImage(cmd, m_offscreenImage,
                        m_offscreenLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    // MSAA color target — один переход после (пере)создания
    const bool useMsaa = m_msaa != VK_SAMPLE_COUNT_1_BIT;
    if (useMsaa && m_msaaColorLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        transitionImage(cmd, m_msaaColorImage,
                        m_msaaColorLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_msaaColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    // View-distance target(ы) → COLOR_ATTACHMENT (1× всегда; MSAA — только при MSAA)
    if (m_viewDistResolveLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        transitionImage(cmd, m_viewDistResolveImage,
                        m_viewDistResolveLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_viewDistResolveLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (useMsaa && m_viewDistMsaaLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        transitionImage(cmd, m_viewDistImage,
                        m_viewDistMsaaLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_viewDistMsaaLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Переводим depth image: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL
    transitionImage(cmd, m_depthImage,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT);

    // 6. Начинаем dynamic rendering (VK_KHR_dynamic_rendering, core 1.3)
    VkClearValue colorClear{{{ 0.012f, 0.006f, 0.006f, 1.0f }}};  // мрак, почти чёрный
    VkClearValue depthClear{ .depthStencil = {1.0f, 0} };

    // MSAA: рендер в мультисэмпловый таргет, resolve → offscreen тем же пассом
    VkRenderingAttachmentInfo colorAttach{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = useMsaa ? m_msaaColorView : m_offscreenView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = useMsaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView   = useMsaa ? m_offscreenView : VK_NULL_HANDLE,
        .resolveImageLayout = useMsaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = useMsaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                               : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = colorClear,
    };
    // Attach 1 — расстояние камера→фрагмент (R16F). Фон/небо = «бесконечность».
    VkClearValue distClear{{{ 1000.f, 0.f, 0.f, 0.f }}};
    VkRenderingAttachmentInfo distAttach{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = useMsaa ? m_viewDistView : m_viewDistResolveView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = useMsaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView   = useMsaa ? m_viewDistResolveView : VK_NULL_HANDLE,
        .resolveImageLayout = useMsaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = useMsaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                               : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = distClear,
    };
    VkRenderingAttachmentInfo colorAttachments[2] = { colorAttach, distAttach };
    VkRenderingAttachmentInfo depthAttach{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue  = depthClear,
    };
    VkRenderingInfo renderInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, m_renderExtent},
        .layerCount           = 1,
        .colorAttachmentCount = 2,
        .pColorAttachments    = colorAttachments,
        .pDepthAttachment     = &depthAttach,
    };
    vkCmdBeginRendering(cmd, &renderInfo);

    // 7. Pipeline state
    VkViewport viewport{
        .x = 0.f, .y = 0.f,
        .width    = (float)m_renderExtent.width,
        .height   = (float)m_renderExtent.height,
        .minDepth = 0.f, .maxDepth = 1.f,
    };
    VkRect2D scissor{ {0, 0}, m_renderExtent };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind scene pipeline (RT-вариант при включённых трассированных тенях)
    const VkPipeline scenePipe = m_rtShadows ? m_scenePipelineRT : m_scenePipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_scenePipelineLayout, 0, 1, &m_descSets[m_currentFrame], 0, nullptr);

    m_frameValid = true;
}

// ─── updateCamera / updateLights ──────────────────────────────────────────────
void VulkanContext::updateCamera(const CameraData& cam) {
    std::memcpy(m_cameraMaps[m_currentFrame], &cam, sizeof(CameraData));
    // Для реконструкции первичных лучей в path-traced режиме
    m_invViewProj = glm::inverse(cam.proj * cam.view);
}

void VulkanContext::updateLights(const LightsData& lights) {
    std::memcpy(m_lightsMaps[m_currentFrame], &lights, sizeof(LightsData));
}

// ─── drawMesh ─────────────────────────────────────────────────────────────────
void VulkanContext::drawMesh(
    VkBuffer       vbo,
    VkBuffer       ibo,
    uint32_t       indexCount,
    const MeshPC&  pc
) {
    if (m_pathTraced) return;  // в path-traced режиме сцена трассируется целиком

    auto cmd = m_cmdBuffers[m_currentFrame];

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
    vkCmdBindIndexBuffer(cmd, ibo, 0, VK_INDEX_TYPE_UINT32);

    vkCmdPushConstants(cmd, m_scenePipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPC), &pc);

    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

// ─── Frame: endFrame ──────────────────────────────────────────────────────────
void VulkanContext::endFrame() {
    if (!m_frameValid) return;

    auto cmd = m_cmdBuffers[m_currentFrame];

    // 1. Завершаем UI pass (открытый в beginUIPass → composite + ImGui)
    vkCmdEndRendering(cmd);

    // 2. Offscreen → COLOR_ATTACHMENT (готов к следующему beginFrame без перехода)
    transitionImage(cmd, m_offscreenImage,
                    m_offscreenLayout,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_offscreenLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // 3. Swapchain: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
    transitionImage(cmd, m_scImages[m_currentImageIdx],
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vkEndCommandBuffer(cmd);

    // 3. Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &m_imageAvailable[m_currentFrame],
        .pWaitDstStageMask    = &waitStage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &m_renderFinished[m_currentImageIdx],
    };
    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit failed");

    // 4. Present
    VkPresentInfoKHR presentInfo{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &m_renderFinished[m_currentImageIdx],
        .swapchainCount     = 1,
        .pSwapchains        = &m_swapchain,
        .pImageIndices      = &m_currentImageIdx,
    };
    VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_needsRecreate = true;  // пересоздадим в начале следующего кадра
    else if (result != VK_SUCCESS)
        throw std::runtime_error("vkQueuePresentKHR failed");

    m_currentFrame = (m_currentFrame + 1) % kMaxFrames;
}

// ─── setVsync ─────────────────────────────────────────────────────────────────
void VulkanContext::setVsync(bool vsync) {
    if (vsync == m_vsync) return;
    m_vsync         = vsync;
    m_needsRecreate = true;
}

// ─── setMsaa ──────────────────────────────────────────────────────────────────
void VulkanContext::setMsaa(VkSampleCountFlagBits samples) {
    if (samples > m_maxMsaa) samples = m_maxMsaa;
    if (samples == m_msaa) return;
    m_msaa          = samples;
    m_needsRecreate = true;  // recreateSwapchain пересоздаст таргеты + пайплайн
}

// ─── Instance ─────────────────────────────────────────────────────────────────
void VulkanContext::createInstance() {
    if (kValidation && !checkValidationLayers())
        throw std::runtime_error("Validation layers недоступны");

    VkApplicationInfo appInfo{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "Podjezd",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName        = "Podjezd Engine",
        .engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_4,
    };

    auto extensions = Window::requiredVulkanExtensions();
    if (kValidation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT debugCI{
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT    |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugCallback,
    };

    VkInstanceCreateInfo ci{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = kValidation ? &debugCI : nullptr,
        .pApplicationInfo        = &appInfo,
        .enabledLayerCount       = kValidation ? (uint32_t)kValidationLayers.size() : 0u,
        .ppEnabledLayerNames     = kValidation ? kValidationLayers.data() : nullptr,
        .enabledExtensionCount   = (uint32_t)extensions.size(),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS)
        throw std::runtime_error("vkCreateInstance failed");

    Logger::info("Vulkan 1.4 instance created");
}

// ─── Debug messenger ──────────────────────────────────────────────────────────
void VulkanContext::setupDebugMessenger() {
    m_fpCreateDebugMessenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    m_fpDestroyDebugMessenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));

    if (!m_fpCreateDebugMessenger)
        throw std::runtime_error("VK_EXT_debug_utils недоступен");

    VkDebugUtilsMessengerCreateInfoEXT ci{
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT    |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugCallback,
    };
    if (m_fpCreateDebugMessenger(m_instance, &ci, nullptr, &m_debugMessenger) != VK_SUCCESS)
        throw std::runtime_error("Не удалось создать debug messenger");
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*                                       /*userData*/
) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        Logger::error("[VK] {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        Logger::warn("[VK] {}", data->pMessage);
    else
        Logger::debug("[VK] {}", data->pMessage);
    return VK_FALSE;
}

// ─── Surface ──────────────────────────────────────────────────────────────────
void VulkanContext::createSurface(const Window& window) {
    if (glfwCreateWindowSurface(m_instance, window.handle(), nullptr, &m_surface) != VK_SUCCESS)
        throw std::runtime_error("glfwCreateWindowSurface failed");
}

// ─── Physical device ──────────────────────────────────────────────────────────
void VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("Vulkan GPU не найден");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (auto dev : devices)
        if (isDeviceSuitable(dev)) { m_physDevice = dev; break; }

    if (m_physDevice == VK_NULL_HANDLE)
        throw std::runtime_error("Подходящий GPU не найден (требуется Vulkan 1.4)");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physDevice, &props);

    // Максимальный MSAA, поддерживаемый и color, и depth таргетами
    const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                      props.limits.framebufferDepthSampleCounts;
    if      (counts & VK_SAMPLE_COUNT_8_BIT) m_maxMsaa = VK_SAMPLE_COUNT_8_BIT;
    else if (counts & VK_SAMPLE_COUNT_4_BIT) m_maxMsaa = VK_SAMPLE_COUNT_4_BIT;
    else if (counts & VK_SAMPLE_COUNT_2_BIT) m_maxMsaa = VK_SAMPLE_COUNT_2_BIT;

    Logger::info("GPU: {} (MSAA up to {}x)", props.deviceName, (int)m_maxMsaa);
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice dev) const {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    if (props.apiVersion < VK_API_VERSION_1_4) return false;
    if (!findQueueFamilies(dev).isComplete()) return false;

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, avail.data());

    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& e : avail) required.erase(e.extensionName);
    if (!required.empty()) return false;

    auto sc = querySwapchainSupport(dev);
    return !sc.formats.empty() && !sc.presentModes.empty();
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice dev) const {
    QueueFamilyIndices idx;
    uint32_t           count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        const auto& f = families[i];
        if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) idx.graphics = i;
        if (f.queueFlags & VK_QUEUE_COMPUTE_BIT)  idx.compute  = i;
        if (f.queueFlags & VK_QUEUE_TRANSFER_BIT) idx.transfer = i;

        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &present);
        if (present) idx.present = i;

        if (idx.isComplete()) break;
    }
    return idx;
}

SwapchainSupport VulkanContext::querySwapchainSupport(VkPhysicalDevice dev) const {
    SwapchainSupport s;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, m_surface, &s.capabilities);

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, m_surface, &count, nullptr);
    if (count) {
        s.formats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, m_surface, &count, s.formats.data());
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, m_surface, &count, nullptr);
    if (count) {
        s.presentModes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, m_surface, &count, s.presentModes.data());
    }
    return s;
}

// ─── Logical device ───────────────────────────────────────────────────────────
void VulkanContext::createLogicalDevice() {
    m_queueIdx = findQueueFamilies(m_physDevice);

    std::set<uint32_t> uniqueQueues = {
        m_queueIdx.graphics.value(), m_queueIdx.compute.value(), m_queueIdx.present.value(),
    };
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCIs;
    for (uint32_t family : uniqueQueues)
        queueCIs.push_back({
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family,
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        });

    // RT-расширения включаем только если GPU их публикует
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateDeviceExtensionProperties(m_physDevice, nullptr, &extCount, avail.data());

    bool rayQuery = true;
    for (const char* need : kRayQueryExtensions) {
        bool found = false;
        for (const auto& e : avail)
            if (std::strcmp(need, e.extensionName) == 0) { found = true; break; }
        if (!found) { rayQuery = false; break; }
    }

    std::vector<const char*> extensions = kDeviceExtensions;
    if (rayQuery)
        extensions.insert(extensions.end(),
                          kRayQueryExtensions.begin(), kRayQueryExtensions.end());

    // RT-пайплайн (path-traced режим) — только поверх ray_query/accel structure
    bool rtPipeline = false;
    if (rayQuery) {
        for (const auto& e : avail)
            if (std::strcmp(kRayTracingPipelineExtension, e.extensionName) == 0) {
                rtPipeline = true;
                break;
            }
        if (rtPipeline) extensions.push_back(kRayTracingPipelineExtension);
    }

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeat{
        .sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .rayTracingPipeline = VK_TRUE,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeat{
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext    = rtPipeline ? &rtPipelineFeat : nullptr,
        .rayQuery = VK_TRUE,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeat{
        .sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext                 = &rayQueryFeat,
        .accelerationStructure = VK_TRUE,
    };

    VkPhysicalDeviceVulkan12Features vk12{
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = rayQuery ? &accelFeat : nullptr,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features vk13{
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = &vk12,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vk13,
    };

    // Device layers мертвы с Vulkan 1.0 — layers задаются только на instance.
    VkDeviceCreateInfo ci{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features2,
        .queueCreateInfoCount    = (uint32_t)queueCIs.size(),
        .pQueueCreateInfos       = queueCIs.data(),
        .enabledExtensionCount   = (uint32_t)extensions.size(),
        .ppEnabledExtensionNames = extensions.data(),
    };
    if (vkCreateDevice(m_physDevice, &ci, nullptr, &m_device) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDevice failed");

    vkGetDeviceQueue(m_device, m_queueIdx.graphics.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueIdx.compute.value(),  0, &m_computeQueue);
    vkGetDeviceQueue(m_device, m_queueIdx.present.value(),  0, &m_presentQueue);

    // AS-функции и состояние RT инкапсулированы в RtSystem
    m_rt.init(m_device, m_physDevice, rayQuery, kMaxFrames);

    // Path tracer пишет в offscreen как в storage image — нужен формат с
    // поддержкой STORAGE_IMAGE; иначе PT недоступен (растеризация не страдает).
    if (rtPipeline) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(m_physDevice, kOffscreenFormat, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            rtPipeline = false;
            Logger::info("RT-пайплайн выключен: offscreen формат без STORAGE_IMAGE");
        }
    }

    // Свойства RT-пайплайна (нужны для выравнивания SBT в PathTracer)
    m_rtPipeline = rtPipeline;
    if (rtPipeline) {
        m_rtPipeProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 props2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &m_rtPipeProps,
        };
        vkGetPhysicalDeviceProperties2(m_physDevice, &props2);
    }

    Logger::info("Logical device: graphics={}, compute={}, present={}, ray_query={}, rt_pipeline={}",
        m_queueIdx.graphics.value(), m_queueIdx.compute.value(),
        m_queueIdx.present.value(), rayQuery, rtPipeline);
}

// ─── Swapchain ────────────────────────────────────────────────────────────────
void VulkanContext::createSwapchain(uint32_t width, uint32_t height) {
    auto sc      = querySwapchainSupport(m_physDevice);
    auto format  = chooseSurfaceFormat(sc.formats);
    auto present = choosePresentMode(sc.presentModes, m_vsync);
    auto extent  = chooseExtent(sc.capabilities, width, height);

    uint32_t imageCount = sc.capabilities.minImageCount + 1;
    if (sc.capabilities.maxImageCount > 0)
        imageCount = std::min(imageCount, sc.capabilities.maxImageCount);

    uint32_t queueIdx[]  = { m_queueIdx.graphics.value(), m_queueIdx.present.value() };
    bool     sharedQueue = queueIdx[0] == queueIdx[1];

    VkSwapchainCreateInfoKHR ci{
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = m_surface,
        .minImageCount    = imageCount,
        .imageFormat      = format.format,
        .imageColorSpace  = format.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode      = sharedQueue ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = sharedQueue ? 0u : 2u,
        .pQueueFamilyIndices   = sharedQueue ? nullptr : queueIdx,
        .preTransform   = sc.capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode    = present,
        .clipped        = VK_TRUE,
    };
    if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSwapchainKHR failed");

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    m_scImages.resize(count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_scImages.data());

    m_scFormat = format.format;
    m_scExtent = extent;

    // Разрешение рендера: масштабируется апскейлером, минимум 16px
    const float scale = (m_upscaler == Upscaler::Off) ? 1.f : m_renderScale;
    m_renderExtent = {
        std::max(16u, (uint32_t)(extent.width  * scale + 0.5f)),
        std::max(16u, (uint32_t)(extent.height * scale + 0.5f)),
    };

    Logger::info("Swapchain {}x{} ({} images, vsync={}, render {}x{})",
                 extent.width, extent.height, count, m_vsync,
                 m_renderExtent.width, m_renderExtent.height);
}

// ─── recreateSwapchain ────────────────────────────────────────────────────────
void VulkanContext::recreateSwapchain() {
    // Свёрнутое окно: framebuffer 0×0 — ждём восстановления
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window->handle(), &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(m_window->handle(), &w, &h);
    }

    vkDeviceWaitIdle(m_device);

    // ── Уничтожаем всё, что зависит от размера/количества образов ─────────────
    vkDestroyImageView(m_device, m_blurTempView, nullptr);
    vkFreeMemory(m_device, m_blurTempMemory, nullptr);
    vkDestroyImage(m_device, m_blurTempImage, nullptr);
    vkDestroyImageView(m_device, m_offscreenView, nullptr);
    vkFreeMemory(m_device, m_offscreenMemory, nullptr);
    vkDestroyImage(m_device, m_offscreenImage, nullptr);

    if (m_msaaColorImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_msaaColorView, nullptr);
        vkFreeMemory(m_device, m_msaaColorMemory, nullptr);
        vkDestroyImage(m_device, m_msaaColorImage, nullptr);
        m_msaaColorImage  = VK_NULL_HANDLE;
        m_msaaColorMemory = VK_NULL_HANDLE;
        m_msaaColorView   = VK_NULL_HANDLE;
    }
    m_msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // View-distance targets (пересоздаются в createOffscreenResources ниже)
    vkDestroyImageView(m_device, m_viewDistResolveView, nullptr);
    vkFreeMemory(m_device, m_viewDistResolveMemory, nullptr);
    vkDestroyImage(m_device, m_viewDistResolveImage, nullptr);
    if (m_viewDistImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_viewDistView, nullptr);
        vkFreeMemory(m_device, m_viewDistMemory, nullptr);
        vkDestroyImage(m_device, m_viewDistImage, nullptr);
        m_viewDistImage  = VK_NULL_HANDLE;
        m_viewDistMemory = VK_NULL_HANDLE;
        m_viewDistView   = VK_NULL_HANDLE;
    }

    vkDestroyImageView(m_device, m_depthView, nullptr);
    vkFreeMemory(m_device, m_depthMemory, nullptr);
    vkDestroyImage(m_device, m_depthImage, nullptr);

    for (auto v : m_scImageViews) vkDestroyImageView(m_device, v, nullptr);
    m_scImageViews.clear();

    for (auto sem : m_renderFinished) vkDestroySemaphore(m_device, sem, nullptr);
    m_renderFinished.clear();

    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;

    // ── Пересоздаём ───────────────────────────────────────────────────────────
    createSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    createImageViews();
    createDepthResources();

    m_offscreenLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_blurTempLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    createOffscreenResources();
    writeBlurDescriptors();    // новые image views в существующие descriptor sets
    writeVisionDescriptors();  // offscreen/blurTemp/viewDist для defocus-прохода

    // Path tracer пишет в offscreen — перепривязать его view к дескрипторам
    if (m_pt.ready() && m_rt.ready())
        for (uint32_t i = 0; i < kMaxFrames; ++i)
            m_pt.configure(i, m_rt.tlas(i), m_cameraUBOs[i].handle, m_lightsUBOs[i].handle,
                           m_rt.geoTable(), m_rt.geoTableSize(), m_offscreenView);

    destroyUpscaleTargets();
    createUpscaleTargets();  // full-res цепочка + дескрипторы + константы

    // renderFinished — per swapchain image; количество могло измениться
    VkSemaphoreCreateInfo semCI{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    m_renderFinished.resize(m_scImages.size());
    for (auto& sem : m_renderFinished)
        if (vkCreateSemaphore(m_device, &semCI, nullptr, &sem) != VK_SUCCESS)
            throw std::runtime_error("Не удалось создать renderFinished semaphore");

    // Scene pipeline фиксирует rasterizationSamples — пересоздаём при смене MSAA.
    // Остальные пайплайны не трогаем: viewport/scissor динамические, форматы стабильны.
    if (m_pipelineSamples != m_msaa) {
        vkDestroyPipeline(m_device, m_scenePipelineRT, nullptr);
        m_scenePipelineRT = VK_NULL_HANDLE;
        vkDestroyPipeline(m_device, m_scenePipeline, nullptr);
        vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr);
        createScenePipeline();
    }
}

void VulkanContext::createImageViews() {
    m_scImageViews.resize(m_scImages.size());
    for (size_t i = 0; i < m_scImages.size(); ++i) {
        VkImageViewCreateInfo ci{
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = m_scImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = m_scFormat,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0, .levelCount     = 1,
                .baseArrayLayer = 0, .layerCount     = 1,
            },
        };
        if (vkCreateImageView(m_device, &ci, nullptr, &m_scImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImageView failed");
    }
}

// ─── Sync objects ─────────────────────────────────────────────────────────────
void VulkanContext::createSyncObjects() {
    // imageAvailable: per frame-in-flight (сигналится acquire, потребляется submit)
    // renderFinished: per swapchain IMAGE — один образ может быть в presentation долго,
    //                 поэтому семафор нельзя переиспользовать до re-acquire того же образа.
    // inFlightFences: per frame-in-flight
    m_imageAvailable.resize(kMaxFrames);
    m_renderFinished.resize(m_scImages.size());
    m_inFlightFences.resize(kMaxFrames);

    VkSemaphoreCreateInfo semCI  { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fenceCI{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        if (vkCreateSemaphore(m_device, &semCI,   nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
            vkCreateFence    (m_device, &fenceCI, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
            throw std::runtime_error("Не удалось создать sync objects");
    }
    for (uint32_t i = 0; i < m_renderFinished.size(); ++i) {
        if (vkCreateSemaphore(m_device, &semCI, nullptr, &m_renderFinished[i]) != VK_SUCCESS)
            throw std::runtime_error("Не удалось создать renderFinished semaphore");
    }
}

// ─── Command pool / buffers ───────────────────────────────────────────────────
void VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo ci{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_queueIdx.graphics.value(),
    };
    if (vkCreateCommandPool(m_device, &ci, nullptr, &m_cmdPool) != VK_SUCCESS)
        throw std::runtime_error("vkCreateCommandPool failed");
}

void VulkanContext::createCommandBuffers() {
    m_cmdBuffers.resize(kMaxFrames);
    VkCommandBufferAllocateInfo alloc{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = m_cmdPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = kMaxFrames,
    };
    if (vkAllocateCommandBuffers(m_device, &alloc, m_cmdBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateCommandBuffers failed");
}

// ─── Depth resources ──────────────────────────────────────────────────────────
void VulkanContext::createDepthResources() {
    VkImageCreateInfo imgCI{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = kDepthFormat,
        .extent        = {m_renderExtent.width, m_renderExtent.height, 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = m_msaa,  // depth живёт только в scene pass — совпадает с MSAA
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(m_device, &imgCI, nullptr, &m_depthImage) != VK_SUCCESS)
        throw std::runtime_error("vkCreateImage (depth) failed");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_depthImage, &req);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physDevice, &memProps);

    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX)
        throw std::runtime_error("Нет подходящей памяти для depth image");

    VkMemoryAllocateInfo alloc{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = memType,
    };
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_depthMemory) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateMemory (depth) failed");

    vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0);

    VkImageViewCreateInfo viewCI{
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = m_depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = kDepthFormat,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0, .levelCount     = 1,
            .baseArrayLayer = 0, .layerCount     = 1,
        },
    };
    if (vkCreateImageView(m_device, &viewCI, nullptr, &m_depthView) != VK_SUCCESS)
        throw std::runtime_error("vkCreateImageView (depth) failed");

    Logger::info("Depth buffer ready (D32_SFLOAT, {}x{}, {}x MSAA)",
                 m_renderExtent.width, m_renderExtent.height, (int)m_msaa);
}

// ─── Camera + Lights UBOs ─────────────────────────────────────────────────────
void VulkanContext::createCameraUBOs() {
    m_cameraUBOs.resize(kMaxFrames);
    m_cameraMaps.resize(kMaxFrames);
    m_lightsUBOs.resize(kMaxFrames);
    m_lightsMaps.resize(kMaxFrames);
    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        m_cameraUBOs[i] = GpuBuffer::createUniform(m_physDevice, m_device, sizeof(CameraData));
        m_cameraMaps[i] = m_cameraUBOs[i].map(m_device);
        m_lightsUBOs[i] = GpuBuffer::createUniform(m_physDevice, m_device, sizeof(LightsData));
        m_lightsMaps[i] = m_lightsUBOs[i].map(m_device);
    }
    // Инициализируем нулевыми данными, чтобы первый кадр не был мусором
    CameraData identity{};
    identity.view     = glm::mat4(1.0f);
    identity.proj     = glm::mat4(1.0f);
    identity.position = glm::vec4(0.0f);
    LightsData noLights{};
    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        std::memcpy(m_cameraMaps[i], &identity, sizeof(CameraData));
        std::memcpy(m_lightsMaps[i], &noLights, sizeof(LightsData));
    }
}

// ─── Descriptor sets ──────────────────────────────────────────────────────────
void VulkanContext::createDescriptors() {
    // Layout: binding 0 = Camera UBO (vert+frag), binding 1 = Lights UBO (frag),
    //         binding 2 = TLAS (frag; только при поддержке ray query)
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 2,
            .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 3,  // geometry table (vboAddr/iboAddr per instance) для GI/отражений
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo layoutCI{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = m_rt.supported() ? 4u : 2u,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_descSetLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorSetLayout failed");

    // Pool
    VkDescriptorPoolSize poolSizes[] = {
        {
            .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = kMaxFrames * 2,  // camera + lights на кадр
        },
        {
            .type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .descriptorCount = kMaxFrames,
        },
        {
            .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // geometry table
            .descriptorCount = kMaxFrames,
        },
    };
    VkDescriptorPoolCreateInfo poolCI{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = kMaxFrames,
        .poolSizeCount = m_rt.supported() ? 3u : 1u,
        .pPoolSizes    = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool failed");

    // Allocate kMaxFrames sets
    std::vector<VkDescriptorSetLayout> layouts(kMaxFrames, m_descSetLayout);
    m_descSets.resize(kMaxFrames);
    VkDescriptorSetAllocateInfo allocInfo{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_descPool,
        .descriptorSetCount = kMaxFrames,
        .pSetLayouts        = layouts.data(),
    };
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descSets.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateDescriptorSets failed");

    // Write: каждый set указывает на свои UBO буферы (камера + свет)
    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        VkDescriptorBufferInfo camInfo{
            .buffer = m_cameraUBOs[i].handle,
            .offset = 0,
            .range  = sizeof(CameraData),
        };
        VkDescriptorBufferInfo lightsInfo{
            .buffer = m_lightsUBOs[i].handle,
            .offset = 0,
            .range  = sizeof(LightsData),
        };
        VkWriteDescriptorSet writes[] = {
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = m_descSets[i],
                .dstBinding      = 0,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo     = &camInfo,
            },
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = m_descSets[i],
                .dstBinding      = 1,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo     = &lightsInfo,
            },
        };
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    }
    Logger::info("Descriptor sets ready ({} frames)", kMaxFrames);
}

// ─── Scene pipeline ───────────────────────────────────────────────────────────
VkShaderModule VulkanContext::loadShader(const std::string& path) const {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Не удалось открыть шейдер: " + path);

    auto size = (size_t)file.tellg();
    std::vector<uint32_t> code(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), (std::streamsize)size);

    VkShaderModuleCreateInfo ci{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code.data(),
    };
    VkShaderModule mod;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("vkCreateShaderModule failed: " + path);
    return mod;
}

void VulkanContext::createScenePipeline() {
    auto vert = loadShader(m_shaderDir + "scene.vert.spv");
    auto frag = loadShader(m_shaderDir + "scene.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName  = "main",
        },
    };

    // Vertex input: одно binding, stride = sizeof(Vertex)
    auto binding   = Vertex::bindingDescription();
    auto attribs   = Vertex::attributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size()),
        .pVertexAttributeDescriptions    = attribs.data(),
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewportState{
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster{
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_BACK_BIT,
        // GLM right-handed + proj[1][1]*=-1 сохраняет CCW-направление в screen space
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo msaa{
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = m_msaa,
    };
    // Depth test
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp   = VK_COMPARE_OP_LESS,
    };
    // Attach 0 — цвет (RGBA), attach 1 — расстояние (R16F). Маски идентичны:
    // без independentBlend все attachment'ы обязаны совпадать, а лишние каналы
    // одноканального R16F всё равно игнорируются.
    VkPipelineColorBlendAttachmentState blendAttach[2] = {
        {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        },
        {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        },
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments    = blendAttach,
    };
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamicStates,
    };

    // Pipeline layout: set 0 = camera UBO, push constant = MeshPC (model + normalMatrix)
    VkPushConstantRange pcRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(MeshPC),  // 128 байт — гарантированный минимум Vulkan
    };
    VkPipelineLayoutCreateInfo layoutCI{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &m_descSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pcRange,
    };
    if (vkCreatePipelineLayout(m_device, &layoutCI, nullptr, &m_scenePipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout (scene) failed");

    const VkFormat sceneColorFormats[2] = { kOffscreenFormat, kViewDistFormat };
    VkPipelineRenderingCreateInfo renderingCI{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 2,
        .pColorAttachmentFormats = sceneColorFormats,
        .depthAttachmentFormat   = kDepthFormat,
    };

    VkGraphicsPipelineCreateInfo pipelineCI{
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &renderingCI,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState      = &viewportState,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pDepthStencilState  = &depthStencil,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = m_scenePipelineLayout,
    };
    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_scenePipeline) != VK_SUCCESS)
        throw std::runtime_error("vkCreateGraphicsPipelines (scene) failed");

    // Вариант с RT-тенями: тот же state, фрагментный шейдер с ray query
    if (m_rt.supported()) {
        auto fragRT = loadShader(m_shaderDir + "scene_rt.frag.spv");
        stages[1].module = fragRT;
        if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineCI, nullptr, &m_scenePipelineRT) != VK_SUCCESS)
            throw std::runtime_error("vkCreateGraphicsPipelines (scene RT) failed");
        vkDestroyShaderModule(m_device, fragRT, nullptr);
    }

    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);

    m_pipelineSamples = m_msaa;
    Logger::info("Scene pipeline ready ({}x MSAA{})", (int)m_msaa,
                 m_rt.supported() ? ", RT variant" : "");
}

// ─── Image layout transition (synchronization2, core 1.3) ────────────────────
namespace {

struct StageAccess {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2        access;
};

// Первый scope барьера: чего ждать и какие записи сделать доступными.
StageAccess barrierSrc(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            // Содержимое отбрасывается — нужна только execution dependency
            // против предыдущего использования образа (WAR), без flush кэшей.
            return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            // Чтение (fragment или compute) — flush не нужен, только execution dependency.
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_NONE};
        case VK_IMAGE_LAYOUT_GENERAL:
            // Storage write из compute (upscale) или ray tracing (path-traced режим)
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        default:
            return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT};
    }
}

// Второй scope барьера: кто ждёт и какие кэши инвалидировать.
StageAccess barrierDst(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            // Дальше синхронизирует семафор сабмита — второй scope пуст.
            return {VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE};
        default:
            return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    }
}

}  // namespace

void VulkanContext::transitionImage(
    VkCommandBuffer    cmd,
    VkImage            image,
    VkImageLayout      from,
    VkImageLayout      to,
    VkImageAspectFlags aspect
) {
    const StageAccess src = barrierSrc(from);
    const StageAccess dst = barrierDst(to);

    VkImageMemoryBarrier2 barrier{
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = src.stage,
        .srcAccessMask = src.access,
        .dstStageMask  = dst.stage,
        .dstAccessMask = dst.access,
        .oldLayout     = from,
        .newLayout     = to,
        .image         = image,
        .subresourceRange = {
            .aspectMask     = aspect,
            .baseMipLevel   = 0,
            .levelCount     = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    VkDependencyInfo dep{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ─── createOffscreenResources ─────────────────────────────────────────────────
void VulkanContext::createOffscreenResources() {
    auto createColorImage = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                                VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                VkFormat format = kOffscreenFormat) {
        VkImageCreateInfo imgCI{
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = format,
            .extent        = {m_renderExtent.width, m_renderExtent.height, 1},
            .mipLevels     = 1,
            .arrayLayers   = 1,
            .samples       = samples,
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = usage,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(m_device, &imgCI, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImage (offscreen) failed");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, image, &req);

        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(m_physDevice, &memProps);

        uint32_t memType = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i; break;
            }
        }
        if (memType == UINT32_MAX)
            throw std::runtime_error("Нет памяти для offscreen image");

        VkMemoryAllocateInfo alloc{
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = req.size,
            .memoryTypeIndex = memType,
        };
        if (vkAllocateMemory(m_device, &alloc, nullptr, &memory) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory (offscreen) failed");
        vkBindImageMemory(m_device, image, memory, 0);

        VkImageViewCreateInfo viewCI{
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = format,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0, .levelCount     = 1,
                .baseArrayLayer = 0, .layerCount     = 1,
            },
        };
        if (vkCreateImageView(m_device, &viewCI, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImageView (offscreen) failed");
    };

    constexpr VkImageUsageFlags kSampledTarget =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // Path-traced режим пишет в offscreen как в storage image (imageStore из rgen)
    const VkImageUsageFlags offscreenUsage =
        kSampledTarget | (m_rtPipeline ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    createColorImage(m_offscreenImage, m_offscreenMemory, m_offscreenView,
                     VK_SAMPLE_COUNT_1_BIT, offscreenUsage);
    createColorImage(m_blurTempImage,  m_blurTempMemory,  m_blurTempView,
                     VK_SAMPLE_COUNT_1_BIT, kSampledTarget);

    // MSAA color: сцена рендерится сюда, resolve → m_offscreenImage
    if (m_msaa != VK_SAMPLE_COUNT_1_BIT)
        createColorImage(m_msaaColorImage, m_msaaColorMemory, m_msaaColorView,
                         m_msaa, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    // View-distance: 1× sampleable-таргет (читает defocus-проход) + при MSAA —
    // отдельный MSAA-таргет, который резолвится в него тем же scene pass'ом.
    m_viewDistResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_viewDistMsaaLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    createColorImage(m_viewDistResolveImage, m_viewDistResolveMemory, m_viewDistResolveView,
                     VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     kViewDistFormat);
    if (m_msaa != VK_SAMPLE_COUNT_1_BIT)
        createColorImage(m_viewDistImage, m_viewDistMemory, m_viewDistView,
                         m_msaa, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, kViewDistFormat);
}

// ─── createBlurPipeline ───────────────────────────────────────────────────────
void VulkanContext::createBlurPipeline() {
    // ── Sampler ───────────────────────────────────────────────────────────────
    VkSamplerCreateInfo samplerCI{
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(m_device, &samplerCI, nullptr, &m_blurSampler) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSampler (blur) failed");

    // ── Descriptor set layout: binding 0 = sampler2D ─────────────────────────
    VkDescriptorSetLayoutBinding samplerBinding{
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo layoutCI{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &samplerBinding,
    };
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_blurDescSetLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorSetLayout (blur) failed");

    // ── Descriptor pool (2 sets: offscreen + temp) ────────────────────────────
    VkDescriptorPoolSize poolSize{
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo poolCI{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 2,
        .poolSizeCount = 1,
        .pPoolSizes    = &poolSize,
    };
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_blurDescPool) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool (blur) failed");

    // ── Allocate 2 descriptor sets ────────────────────────────────────────────
    std::array<VkDescriptorSetLayout, 2> layouts = {m_blurDescSetLayout, m_blurDescSetLayout};
    VkDescriptorSetAllocateInfo dsAlloc{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_blurDescPool,
        .descriptorSetCount = 2,
        .pSetLayouts        = layouts.data(),
    };
    std::array<VkDescriptorSet, 2> sets{};
    if (vkAllocateDescriptorSets(m_device, &dsAlloc, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateDescriptorSets (blur) failed");
    m_blurDescSetOffscreen = sets[0];
    m_blurDescSetTemp      = sets[1];

    // ── Write image views into descriptor sets ────────────────────────────────
    writeBlurDescriptors();

    // ── Pipeline layout ───────────────────────────────────────────────────────
    // Push constants only for blur pipeline (direction + radius = 12 bytes)
    VkPushConstantRange blurPC{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(float) * 2 + sizeof(int32_t),  // vec2 + int = 12 bytes
    };
    VkPipelineLayoutCreateInfo blurLayoutCI{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &m_blurDescSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &blurPC,
    };
    if (vkCreatePipelineLayout(m_device, &blurLayoutCI, nullptr, &m_blurPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout (blur) failed");

    // ── Shared pipeline state (no vertex input, no depth) ────────────────────
    VkPipelineVertexInputStateCreateInfo emptyVertex{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo triList{
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewportState{
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster{
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .lineWidth   = 1.f,
    };
    VkPipelineMultisampleStateCreateInfo msaa{
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo noDepth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState blendAttach{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blendAttach,
    };
    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynStates.size()),
        .pDynamicStates    = dynStates.data(),
    };

    VkShaderModule fsVert = loadShader(m_shaderDir + "fullscreen.vert.spv");

    // ── Blur pipeline (writes to kOffscreenFormat, no depth) ─────────────────
    {
        VkShaderModule blurFrag = loadShader(m_shaderDir + "blur.frag.spv");

        std::array<VkPipelineShaderStageCreateInfo, 2> blurStages = {{
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = fsVert, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = blurFrag, .pName = "main"},
        }};

        VkPipelineRenderingCreateInfo blurRendering{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &kOffscreenFormat,
        };
        VkGraphicsPipelineCreateInfo pCI{
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &blurRendering,
            .stageCount          = 2,
            .pStages             = blurStages.data(),
            .pVertexInputState   = &emptyVertex,
            .pInputAssemblyState = &triList,
            .pViewportState      = &viewportState,
            .pRasterizationState = &raster,
            .pMultisampleState   = &msaa,
            .pDepthStencilState  = &noDepth,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = m_blurPipelineLayout,
        };
        if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pCI, nullptr, &m_blurPipeline) != VK_SUCCESS)
            throw std::runtime_error("vkCreateGraphicsPipelines (blur) failed");

        vkDestroyShaderModule(m_device, blurFrag, nullptr);
    }

    // ── Composite pipeline (writes to swapchain format + depth) ──────────────
    {
        // We reuse m_blurPipelineLayout for composite (it has the push constant range but
        // the composite shader simply ignores push constants — this is valid in Vulkan).

        VkShaderModule compFrag = loadShader(m_shaderDir + "composite.frag.spv");

        std::array<VkPipelineShaderStageCreateInfo, 2> compStages = {{
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = fsVert, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = compFrag, .pName = "main"},
        }};

        // UI pass без depth: он не нужен composite/ImGui, а depth-таргет может быть MSAA
        VkPipelineRenderingCreateInfo compRendering{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = 1,
            .pColorAttachmentFormats = &m_scFormat,
        };
        VkGraphicsPipelineCreateInfo pCI{
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &compRendering,
            .stageCount          = 2,
            .pStages             = compStages.data(),
            .pVertexInputState   = &emptyVertex,
            .pInputAssemblyState = &triList,
            .pViewportState      = &viewportState,
            .pRasterizationState = &raster,
            .pMultisampleState   = &msaa,
            .pDepthStencilState  = &noDepth,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = m_blurPipelineLayout,
        };
        if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pCI, nullptr, &m_compositePipeline) != VK_SUCCESS)
            throw std::runtime_error("vkCreateGraphicsPipelines (composite) failed");

        vkDestroyShaderModule(m_device, compFrag, nullptr);
    }

    vkDestroyShaderModule(m_device, fsVert, nullptr);
}

// ─── buildAccelerationStructures ──────────────────────────────────────────────
// RtSystem строит BLAS/TLAS/geo-table; здесь только привязываем результат
// к дескрипторам (TLAS → binding 2, geometry table → binding 3).
void VulkanContext::buildAccelerationStructures(const std::vector<RtInstance>& instances) {
    m_rt.build(m_graphicsQueue, m_cmdPool, instances);
    if (!m_rt.ready()) return;

    for (uint32_t i = 0; i < kMaxFrames; ++i) {
        VkAccelerationStructureKHR tlas = m_rt.tlas(i);  // свой TLAS на кадр-в-полёте
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures    = &tlas,
        };
        VkDescriptorBufferInfo geoInfo{
            .buffer = m_rt.geoTable(), .offset = 0, .range = m_rt.geoTableSize()};
        VkWriteDescriptorSet writes[] = {
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext           = &asWrite,
                .dstSet          = m_descSets[i],
                .dstBinding      = 2,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            },
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = m_descSets[i],
                .dstBinding      = 3,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo     = &geoInfo,
            },
        };
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    }

    // Path tracer: привязать TLAS/geo/UBO/offscreen для каждого frame-in-flight
    if (m_pt.ready())
        for (uint32_t i = 0; i < kMaxFrames; ++i)
            m_pt.configure(i, m_rt.tlas(i), m_cameraUBOs[i].handle, m_lightsUBOs[i].handle,
                           m_rt.geoTable(), m_rt.geoTableSize(), m_offscreenView);
}

// ─── writeBlurDescriptors ─────────────────────────────────────────────────────
// Привязывает текущие offscreen/blurTemp views к descriptor sets.
// Вызывается при создании и после recreateSwapchain (views пересозданы).
void VulkanContext::writeBlurDescriptors() {
    auto writeSet = [&](VkDescriptorSet ds, VkImageView view) {
        VkDescriptorImageInfo imgInfo{
            .sampler     = m_blurSampler,
            .imageView   = view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = ds,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &imgInfo,
        };
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    };
    writeSet(m_blurDescSetOffscreen, m_offscreenView);
    writeSet(m_blurDescSetTemp,      m_blurTempView);
}

// ─── createVisionPipeline ─────────────────────────────────────────────────────
// Проход симуляции зрения (depth-based defocus). Читает цвет + viewDist (2
// combined image sampler), пишет в kOffscreenFormat-таргет. Переиспользует
// m_blurSampler (linear clamp). Активируется только при visionActive().
void VulkanContext::createVisionPipeline() {
    // ── Descriptor set layout: 0 = color, 1 = viewDist ────────────────────────
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layoutCI{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_visionDescSetLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorSetLayout (vision) failed");

    // ── Pool: 2 набора × 2 сэмплера ───────────────────────────────────────────
    VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 4};
    VkDescriptorPoolCreateInfo poolCI{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 2,
        .poolSizeCount = 1,
        .pPoolSizes    = &poolSize,
    };
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_visionDescPool) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool (vision) failed");

    std::array<VkDescriptorSetLayout, 2> setLayouts = {m_visionDescSetLayout, m_visionDescSetLayout};
    VkDescriptorSetAllocateInfo dsAlloc{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_visionDescPool,
        .descriptorSetCount = 2,
        .pSetLayouts        = setLayouts.data(),
    };
    std::array<VkDescriptorSet, 2> sets{};
    if (vkAllocateDescriptorSets(m_device, &dsAlloc, sets.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateDescriptorSets (vision) failed");
    m_visionDescOffscreen = sets[0];
    m_visionDescTemp      = sets[1];
    writeVisionDescriptors();

    // ── Pipeline layout: push-константы VisionPC (4 float = 16 байт) ───────────
    VkPushConstantRange pcRange{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(float) * 4,
    };
    VkPipelineLayoutCreateInfo plCI{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &m_visionDescSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pcRange,
    };
    if (vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_visionPipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout (vision) failed");

    // ── Fullscreen graphics pipeline (no vertex input, no depth) ──────────────
    VkPipelineVertexInputStateCreateInfo emptyVertex{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo triList{
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.f};
    VkPipelineMultisampleStateCreateInfo msaa{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo noDepth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttach{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blendAttach};
    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynStates.size()),
        .pDynamicStates    = dynStates.data()};

    VkShaderModule fsVert     = loadShader(m_shaderDir + "fullscreen.vert.spv");
    VkShaderModule visionFrag = loadShader(m_shaderDir + "vision.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages = {{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = fsVert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = visionFrag, .pName = "main"},
    }};
    VkPipelineRenderingCreateInfo rendering{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &kOffscreenFormat,
    };
    VkGraphicsPipelineCreateInfo pCI{
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = 2,
        .pStages             = stages.data(),
        .pVertexInputState   = &emptyVertex,
        .pInputAssemblyState = &triList,
        .pViewportState      = &viewportState,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pDepthStencilState  = &noDepth,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = m_visionPipelineLayout,
    };
    if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pCI, nullptr, &m_visionPipeline) != VK_SUCCESS)
        throw std::runtime_error("vkCreateGraphicsPipelines (vision) failed");

    vkDestroyShaderModule(m_device, visionFrag, nullptr);
    vkDestroyShaderModule(m_device, fsVert, nullptr);
    Logger::info("Vision defocus pipeline ready");
}

// ─── writeVisionDescriptors ───────────────────────────────────────────────────
// Привязывает текущие views: color (offscreen/blurTemp) + viewDist. Вызывать при
// создании и после recreateSwapchain (views пересозданы).
void VulkanContext::writeVisionDescriptors() {
    auto writeSet = [&](VkDescriptorSet ds, VkImageView colorView) {
        VkDescriptorImageInfo color{
            .sampler = m_blurSampler, .imageView = colorView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dist{
            .sampler = m_blurSampler, .imageView = m_viewDistResolveView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &color},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &dist},
        };
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    };
    writeSet(m_visionDescOffscreen, m_offscreenView);
    writeSet(m_visionDescTemp,      m_blurTempView);
}

// ─── setMyopia / setHyperopia ─────────────────────────────────────────────────
// Пересчёт зоны резкости и силы дефокуса. Близорукость D → даль резка до 1/D м;
// дальнозоркость D → близь резка от 1/D м. strength масштабирует диоптрии в
// пиксели радиуса CoC (при render-res); подобрано так, чтобы −6 D давало
// выразительное, но не запредельное размытие.
namespace {
constexpr float kVisionDioptersToPixels = 4.0f;   // пикселей CoC на диоптрию
constexpr float kVisionMaxRadius        = 20.0f;  // кламп радиуса CoC, пиксели
void recomputeVision(float myopiaD, float hyperopiaD,
                     float& nearSharp, float& farSharp, float& strength) {
    // Близорукость: дальняя точка = 1/D (за ней размытие). 0 D → ∞ (farSharp=0).
    farSharp  = myopiaD    > 0.01f ? 1.0f / myopiaD    : 0.0f;
    // Дальнозоркость: ближняя точка = 1/D (ближе неё размытие). 0 D → 0 (нет).
    nearSharp = hyperopiaD > 0.01f ? 1.0f / hyperopiaD : 0.0f;
    strength  = (myopiaD > 0.01f || hyperopiaD > 0.01f) ? kVisionDioptersToPixels : 0.0f;
}
}  // namespace

void VulkanContext::setMyopia(float diopters) {
    m_myopiaD = std::clamp(diopters, 0.f, 6.f);
    recomputeVision(m_myopiaD, m_hyperopiaD,
                    m_visionNearSharp, m_visionFarSharp, m_visionStrength);
}

void VulkanContext::setHyperopia(float diopters) {
    m_hyperopiaD = std::clamp(diopters, 0.f, 6.f);
    recomputeVision(m_myopiaD, m_hyperopiaD,
                    m_visionNearSharp, m_visionFarSharp, m_visionStrength);
}

// ─── Pipeline cache ───────────────────────────────────────────────────────────
void VulkanContext::createPipelineCache() {
    // Загружаем сохранённые данные, если есть; драйвер сам валидирует
    // header (vendor/device/UUID) и молча игнорирует несовместимые данные.
    std::vector<char> data;
    std::ifstream file(m_shaderDir + "pipeline_cache.bin", std::ios::binary | std::ios::ate);
    if (file) {
        data.resize(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
    }

    VkPipelineCacheCreateInfo ci{
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data.size(),
        .pInitialData    = data.empty() ? nullptr : data.data(),
    };
    if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        // Кэш — оптимизация, не критичен: пробуем без initial data
        ci.initialDataSize = 0;
        ci.pInitialData    = nullptr;
        if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS)
            m_pipelineCache = VK_NULL_HANDLE;
    }
}

void VulkanContext::savePipelineCache() const {
    if (m_pipelineCache == VK_NULL_HANDLE) return;

    size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS)
        return;

    std::ofstream file(m_shaderDir + "pipeline_cache.bin", std::ios::binary | std::ios::trunc);
    if (file) file.write(data.data(), static_cast<std::streamsize>(size));
}

// ─── Upscalers (AMD FSR 1.0 + NVIDIA Image Scaling) ──────────────────────────

void VulkanContext::setUpscaler(Upscaler mode, float renderScale) {
    renderScale = std::clamp(renderScale, 0.5f, 1.0f);
    if (mode == m_upscaler && renderScale == m_renderScale) return;
    m_upscaler      = mode;
    m_renderScale   = renderScale;
    m_needsRecreate = true;  // таргеты и render extent пересоздаются
}

void VulkanContext::setSharpness(float sharpness) {
    m_sharpness = std::clamp(sharpness, 0.f, 1.f);
    updateUpscaleConstants();
}

// Compute-пайплайны, NIS coef-текстуры и сэмплер — создаются один раз.
void VulkanContext::createUpscalePipelines() {
    // ── FSR: layout = combined sampler + storage image, push 64B ─────────────
    {
        VkDescriptorSetLayoutBinding bindings[] = {
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        };
        VkDescriptorSetLayoutCreateInfo layoutCI{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings    = bindings,
        };
        if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_fsrDescSetLayout) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDescriptorSetLayout (fsr) failed");

        VkPushConstantRange pc{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0,
            .size       = sizeof(uint32_t) * 16,  // EASU con0..3; RCAS использует первые 16B
        };
        VkPipelineLayoutCreateInfo plCI{
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &m_fsrDescSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pc,
        };
        if (vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_fsrPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("vkCreatePipelineLayout (fsr) failed");

        auto makeCompute = [&](const char* file, VkPipelineLayout layout, VkPipeline& pipe) {
            auto mod = loadShader(m_shaderDir + file);
            VkComputePipelineCreateInfo cpCI{
                .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage  = {
                    .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = mod,
                    .pName  = "main",
                },
                .layout = layout,
            };
            if (vkCreateComputePipelines(m_device, m_pipelineCache, 1, &cpCI, nullptr, &pipe) != VK_SUCCESS)
                throw std::runtime_error(std::string("compute pipeline failed: ") + file);
            vkDestroyShaderModule(m_device, mod, nullptr);
        };
        makeCompute("fsr_easu.comp.spv", m_fsrPipelineLayout, m_easuPipeline);
        makeCompute("fsr_rcas.comp.spv", m_fsrPipelineLayout, m_rcasPipeline);

        // ── NIS: UBO + sampler + 3 текстуры + storage ─────────────────────────
        VkDescriptorSetLayoutBinding nisBindings[] = {
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        };
        VkDescriptorSetLayoutCreateInfo nisLayoutCI{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 6,
            .pBindings    = nisBindings,
        };
        if (vkCreateDescriptorSetLayout(m_device, &nisLayoutCI, nullptr, &m_nisDescSetLayout) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDescriptorSetLayout (nis) failed");

        VkPipelineLayoutCreateInfo nisPlCI{
            .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts    = &m_nisDescSetLayout,
        };
        if (vkCreatePipelineLayout(m_device, &nisPlCI, nullptr, &m_nisPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("vkCreatePipelineLayout (nis) failed");

        makeCompute("nis_upscale.comp.spv", m_nisPipelineLayout, m_nisPipeline);
    }

    // ── NIS sampler (linear clamp — требование SDK) ──────────────────────────
    VkSamplerCreateInfo samplerCI{
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(m_device, &samplerCI, nullptr, &m_nisSampler) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSampler (nis) failed");

    // ── NIS config UBO ────────────────────────────────────────────────────────
    m_nisConfigUBO = GpuBuffer::createUniform(m_physDevice, m_device, sizeof(NISConfig));

    // ── NIS coef-текстуры: 2 × (kFilterSize/4 × kPhaseCount) RGBA32F ─────────
    {
        constexpr uint32_t kCoefW = 2;   // kFilterSize(8) / 4 канала
        constexpr uint32_t kCoefH = 64;  // kPhaseCount

        auto createCoefTexture = [&](const void* data, VkImage& img,
                                     VkDeviceMemory& mem, VkImageView& view) {
            VkImageCreateInfo imgCI{
                .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType     = VK_IMAGE_TYPE_2D,
                .format        = VK_FORMAT_R32G32B32A32_SFLOAT,
                .extent        = {kCoefW, kCoefH, 1},
                .mipLevels     = 1,
                .arrayLayers   = 1,
                .samples       = VK_SAMPLE_COUNT_1_BIT,
                .tiling        = VK_IMAGE_TILING_OPTIMAL,
                .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            if (vkCreateImage(m_device, &imgCI, nullptr, &img) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImage (nis coef) failed");

            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(m_device, img, &req);
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(m_physDevice, &memProps);
            uint32_t memType = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
                if ((req.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    memType = i; break;
                }
            VkMemoryAllocateInfo alloc{
                .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize  = req.size,
                .memoryTypeIndex = memType,
            };
            if (vkAllocateMemory(m_device, &alloc, nullptr, &mem) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateMemory (nis coef) failed");
            vkBindImageMemory(m_device, img, mem, 0);

            // Upload: staging buffer → image
            const VkDeviceSize dataSize = kCoefW * kCoefH * 4 * sizeof(float);
            GpuBuffer staging = GpuBuffer::createHostVisible(
                m_physDevice, m_device, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            void* mapped = staging.map(m_device);
            std::memcpy(mapped, data, dataSize);
            staging.unmap(m_device);

            auto cmd = m_cmdBuffers[0];
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(cmd, &begin);
            transitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent      = {kCoefW, kCoefH, 1},
            };
            vkCmdCopyBufferToImage(cmd, staging.handle, img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{
                .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers    = &cmd,
            };
            vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_graphicsQueue);
            staging.destroy(m_device);

            VkImageViewCreateInfo viewCI{
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image    = img,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            if (vkCreateImageView(m_device, &viewCI, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImageView (nis coef) failed");
        };

        // coef_scale / coef_usm — официальные таблицы из NIS_Config.h
        createCoefTexture(coef_scale,
                          m_nisCoefScaleImage, m_nisCoefScaleMemory, m_nisCoefScaleView);
        createCoefTexture(coef_usm,
                          m_nisCoefUsmImage, m_nisCoefUsmMemory, m_nisCoefUsmView);
    }

    // ── Descriptor pool: 3 fsr-style сета + 1 nis + 1 composite(sampler) ─────
    VkDescriptorPoolSize poolSizes[] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 4},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 4},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         .descriptorCount = 1},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER,                .descriptorCount = 1},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          .descriptorCount = 3},
    };
    VkDescriptorPoolCreateInfo poolCI{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 4,
        .poolSizeCount = 5,
        .pPoolSizes    = poolSizes,
    };
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_upscaleDescPool) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool (upscale) failed");

    // Аллокация: EASU, RCAS (fsr layout), NIS, composite (blur layout)
    VkDescriptorSetLayout allocLayouts[] = {
        m_fsrDescSetLayout, m_fsrDescSetLayout, m_nisDescSetLayout, m_blurDescSetLayout,
    };
    VkDescriptorSetAllocateInfo dsAlloc{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_upscaleDescPool,
        .descriptorSetCount = 4,
        .pSetLayouts        = allocLayouts,
    };
    VkDescriptorSet sets[4]{};
    if (vkAllocateDescriptorSets(m_device, &dsAlloc, sets) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateDescriptorSets (upscale) failed");
    m_dsEasu              = sets[0];
    m_dsRcas              = sets[1];
    m_dsNis               = sets[2];
    m_blurDescSetUpscaled = sets[3];

    Logger::info("Upscalers ready: AMD FSR 1.0 (EASU+RCAS), NVIDIA Image Scaling");
}

void VulkanContext::createUpscaleTargets() {
    if (m_upscaler == Upscaler::Off) return;

    auto createFullRes = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo imgCI{
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = kOffscreenFormat,
            .extent        = {m_scExtent.width, m_scExtent.height, 1},
            .mipLevels     = 1,
            .arrayLayers   = 1,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(m_device, &imgCI, nullptr, &img) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImage (upscale) failed");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, img, &req);
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(m_physDevice, &memProps);
        uint32_t memType = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i; break;
            }
        VkMemoryAllocateInfo alloc{
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = req.size,
            .memoryTypeIndex = memType,
        };
        if (vkAllocateMemory(m_device, &alloc, nullptr, &mem) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory (upscale) failed");
        vkBindImageMemory(m_device, img, mem, 0);
        VkImageViewCreateInfo viewCI{
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = kOffscreenFormat,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(m_device, &viewCI, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImageView (upscale) failed");
    };

    createFullRes(m_upscaledImage, m_upscaledMemory, m_upscaledView);
    if (m_upscaler == Upscaler::FSR1)  // temp нужен только для EASU→RCAS
        createFullRes(m_upTempImage, m_upTempMemory, m_upTempView);

    m_upscaledLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_upTempLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    writeUpscaleDescriptors();
    updateUpscaleConstants();
}

void VulkanContext::destroyUpscaleTargets() {
    if (m_upscaledImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_upscaledView, nullptr);
        vkFreeMemory(m_device, m_upscaledMemory, nullptr);
        vkDestroyImage(m_device, m_upscaledImage, nullptr);
        m_upscaledImage = VK_NULL_HANDLE;
        m_upscaledView  = VK_NULL_HANDLE;
    }
    if (m_upTempImage != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_upTempView, nullptr);
        vkFreeMemory(m_device, m_upTempMemory, nullptr);
        vkDestroyImage(m_device, m_upTempImage, nullptr);
        m_upTempImage = VK_NULL_HANDLE;
        m_upTempView  = VK_NULL_HANDLE;
    }
}

void VulkanContext::writeUpscaleDescriptors() {
    auto writeFsrSet = [&](VkDescriptorSet ds, VkImageView in, VkImageView out) {
        VkDescriptorImageInfo inInfo{
            .sampler     = m_blurSampler,
            .imageView   = in,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorImageInfo outInfo{
            .imageView   = out,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet writes[] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &inInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &outInfo},
        };
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    };

    if (m_upscaler == Upscaler::FSR1) {
        writeFsrSet(m_dsEasu, m_offscreenView, m_upTempView);
        writeFsrSet(m_dsRcas, m_upTempView, m_upscaledView);
    }

    if (m_upscaler == Upscaler::NIS) {
        VkDescriptorBufferInfo cfgInfo{
            .buffer = m_nisConfigUBO.handle, .offset = 0, .range = sizeof(NISConfig)};
        VkDescriptorImageInfo samplerInfo{.sampler = m_nisSampler};
        VkDescriptorImageInfo inInfo{
            .imageView = m_offscreenView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo outInfo{
            .imageView = m_upscaledView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo coefScaleInfo{
            .imageView = m_nisCoefScaleView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo coefUsmInfo{
            .imageView = m_nisCoefUsmView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             .pBufferInfo = &cfgInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
             .pImageInfo = &samplerInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 2,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = &inInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 3,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &outInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 4,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = &coefScaleInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_dsNis, .dstBinding = 5,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = &coefUsmInfo},
        };
        vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
    }

    // Composite сэмплит из upscaled (blur-style layout: combined sampler binding 0)
    if (m_upscaler != Upscaler::Off) {
        VkDescriptorImageInfo upInfo{
            .sampler     = m_blurSampler,
            .imageView   = m_upscaledView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = m_blurDescSetUpscaled,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &upInfo,
        };
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void VulkanContext::updateUpscaleConstants() {
    if (m_upscaler == Upscaler::FSR1) {
        // Резкость RCAS в "стопах": 0 = максимум; UI 0..1 → 2..0
        const FsrConstants con = computeFsrConstants(
            m_renderExtent.width, m_renderExtent.height,
            m_scExtent.width, m_scExtent.height,
            (1.f - m_sharpness) * 2.f);
        std::memcpy(m_easuCon, con.easu, sizeof(m_easuCon));
        std::memcpy(m_rcasCon, con.rcas, sizeof(m_rcasCon));
    }
    if (m_upscaler == Upscaler::NIS) {
        NISConfig cfg{};
        NVScalerUpdateConfig(cfg, m_sharpness,
            0, 0, m_renderExtent.width, m_renderExtent.height,
            m_renderExtent.width, m_renderExtent.height,
            0, 0, m_scExtent.width, m_scExtent.height,
            m_scExtent.width, m_scExtent.height,
            NISHDRMode::None);
        void* mapped = m_nisConfigUBO.map(m_device);
        std::memcpy(mapped, &cfg, sizeof(cfg));
        m_nisConfigUBO.unmap(m_device);
    }
}

// Записывает compute-пассы апскейла. Вызывается из beginUIPass:
// offscreen уже в SHADER_READ_ONLY, результат остаётся в m_upscaledImage (SHADER_READ).
void VulkanContext::recordUpscale(VkCommandBuffer cmd) {
    if (m_upscaler == Upscaler::FSR1) {
        // EASU: offscreen → upTemp
        transitionImage(cmd, m_upTempImage, m_upTempLayout, VK_IMAGE_LAYOUT_GENERAL);
        m_upTempLayout = VK_IMAGE_LAYOUT_GENERAL;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_easuPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_fsrPipelineLayout, 0, 1, &m_dsEasu, 0, nullptr);
        vkCmdPushConstants(cmd, m_fsrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(m_easuCon), m_easuCon);
        // Официальный remap FSR: 16×16 пикселей на workgroup
        const uint32_t dx = (m_scExtent.width  + 15) / 16;
        const uint32_t dy = (m_scExtent.height + 15) / 16;
        vkCmdDispatch(cmd, dx, dy, 1);

        // RCAS: upTemp → upscaled
        transitionImage(cmd, m_upTempImage, m_upTempLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_upTempLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transitionImage(cmd, m_upscaledImage, m_upscaledLayout, VK_IMAGE_LAYOUT_GENERAL);
        m_upscaledLayout = VK_IMAGE_LAYOUT_GENERAL;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_rcasPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_fsrPipelineLayout, 0, 1, &m_dsRcas, 0, nullptr);
        vkCmdPushConstants(cmd, m_fsrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(m_rcasCon), m_rcasCon);
        vkCmdDispatch(cmd, dx, dy, 1);
    } else if (m_upscaler == Upscaler::NIS) {
        transitionImage(cmd, m_upscaledImage, m_upscaledLayout, VK_IMAGE_LAYOUT_GENERAL);
        m_upscaledLayout = VK_IMAGE_LAYOUT_GENERAL;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_nisPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_nisPipelineLayout, 0, 1, &m_dsNis, 0, nullptr);
        // NIS: block 32×24 (NISOptimizer для AMD)
        const uint32_t dx = (m_scExtent.width  + 31) / 32;
        const uint32_t dy = (m_scExtent.height + 23) / 24;
        vkCmdDispatch(cmd, dx, dy, 1);
    }

    // Результат → sampled для composite
    transitionImage(cmd, m_upscaledImage, m_upscaledLayout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_upscaledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ─── beginUIPass ──────────────────────────────────────────────────────────────
void VulkanContext::beginUIPass(float blurStrength) {
    auto cmd = m_cmdBuffers[m_currentFrame];

    if (m_pathTraced && m_pt.ready()) {
        // Трассируем сцену целиком в offscreen (storage), затем → SHADER_READ.
        // Растровый pass не открывался — vkCmdEndRendering не вызываем.
        constexpr uint32_t kPtSpp = 2;  // сэмплов за кадр (path tracing тяжёлый)
        transitionImage(cmd, m_offscreenImage, m_offscreenLayout, VK_IMAGE_LAYOUT_GENERAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_GENERAL;
        m_pt.trace(cmd, m_currentFrame, m_renderExtent, m_invViewProj, m_ptFrame++, kPtSpp);
        transitionImage(cmd, m_offscreenImage, m_offscreenLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        // Закрываем scene pass
        vkCmdEndRendering(cmd);
        // offscreen: COLOR_ATTACHMENT → SHADER_READ_ONLY (для sampling)
        transitionImage(cmd, m_offscreenImage, m_offscreenLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ── Симуляция зрения: depth-based defocus (только раст-режим, viewDist есть) ─
    // Два прохода одним пайплайном: gather offscreen→blurTemp с реальными
    // параметрами, затем copy-back blurTemp→offscreen (strength=0 → passthrough).
    // Результат остаётся в offscreen — downstream (blur/upscale/composite) не меняется.
    if (visionActive() && !(m_pathTraced && m_pt.ready())) {
        struct VisionPC { float nearSharp, farSharp, strength, maxRadius; };

        VkViewport vp{
            .x = 0.f, .y = 0.f,
            .width    = static_cast<float>(m_renderExtent.width),
            .height   = static_cast<float>(m_renderExtent.height),
            .minDepth = 0.f, .maxDepth = 1.f,
        };
        VkRect2D sc{{0, 0}, m_renderExtent};

        // viewDist → SHADER_READ (сэмплится обоими проходами)
        transitionImage(cmd, m_viewDistResolveImage, m_viewDistResolveLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_viewDistResolveLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        auto visionPass = [&](VkImageView dstView, VkDescriptorSet srcSet, const VisionPC& pc) {
            VkRenderingAttachmentInfo ca{
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = dstView,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
            };
            VkRenderingInfo ri{
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea           = {{0, 0}, m_renderExtent},
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &ca,
            };
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_visionPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_visionPipelineLayout, 0, 1, &srcSet, 0, nullptr);
            vkCmdPushConstants(cmd, m_visionPipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        };

        // Проход 1 (gather): offscreen (SHADER_READ) → blurTemp (COLOR_ATTACHMENT)
        transitionImage(cmd, m_blurTempImage, m_blurTempLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_blurTempLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        visionPass(m_blurTempView, m_visionDescOffscreen,
                   {m_visionNearSharp, m_visionFarSharp, m_visionStrength, kVisionMaxRadius});

        // blurTemp → SHADER_READ, offscreen → COLOR_ATTACHMENT
        transitionImage(cmd, m_blurTempImage, m_blurTempLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_blurTempLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transitionImage(cmd, m_offscreenImage, m_offscreenLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Проход 2 (copy-back, strength=0 → passthrough): blurTemp → offscreen
        visionPass(m_offscreenView, m_visionDescTemp, {0.f, 0.f, 0.f, 0.f});

        // offscreen → SHADER_READ для downstream (blur/upscale/composite)
        transitionImage(cmd, m_offscreenImage, m_offscreenLayout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_offscreenLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // blurStrength [0,1] → radius [0, 12]; при radius==0 блюр пропускается
    const int blurRadius = static_cast<int>(blurStrength * 12.f + 0.5f);

    if (blurRadius > 0) {
        // blurTemp: предыдущий layout → COLOR_ATTACHMENT
        transitionImage(cmd, m_blurTempImage,
                        m_blurTempLayout,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_blurTempLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkViewport viewport{
            .x = 0.f, .y = 0.f,
            .width    = static_cast<float>(m_renderExtent.width),
            .height   = static_cast<float>(m_renderExtent.height),
            .minDepth = 0.f, .maxDepth = 1.f,
        };
        VkRect2D scissor{{0, 0}, m_renderExtent};

        struct BlurPC { float dirX, dirY; int32_t radius; };

        {  // один сепарабельный H+V проход
            // ── H pass: offscreen (SHADER_READ) → blurTemp (COLOR_ATTACHMENT) ──
            {
                VkClearValue clear{{{ 0.f, 0.f, 0.f, 1.f }}};
                VkRenderingAttachmentInfo ca{
                    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView   = m_blurTempView,
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                    .clearValue  = clear,
                };
                VkRenderingInfo ri{
                    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea           = {{0, 0}, m_renderExtent},
                    .layerCount           = 1,
                    .colorAttachmentCount = 1,
                    .pColorAttachments    = &ca,
                };
                vkCmdBeginRendering(cmd, &ri);
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_blurPipelineLayout, 0, 1, &m_blurDescSetOffscreen, 0, nullptr);
                BlurPC pc{1.f, 0.f, blurRadius};
                vkCmdPushConstants(cmd, m_blurPipelineLayout,
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }

            // blurTemp → SHADER_READ, offscreen → COLOR_ATTACHMENT
            transitionImage(cmd, m_blurTempImage,
                            m_blurTempLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_blurTempLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            transitionImage(cmd, m_offscreenImage,
                            m_offscreenLayout,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            m_offscreenLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // ── V pass: blurTemp (SHADER_READ) → offscreen (COLOR_ATTACHMENT) ──
            {
                VkRenderingAttachmentInfo ca{
                    .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView   = m_offscreenView,
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                };
                VkRenderingInfo ri{
                    .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea           = {{0, 0}, m_renderExtent},
                    .layerCount           = 1,
                    .colorAttachmentCount = 1,
                    .pColorAttachments    = &ca,
                };
                vkCmdBeginRendering(cmd, &ri);
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_blurPipelineLayout, 0, 1, &m_blurDescSetTemp, 0, nullptr);
                BlurPC pc{0.f, 1.f, blurRadius};
                vkCmdPushConstants(cmd, m_blurPipelineLayout,
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }

            // offscreen → SHADER_READ для composite
            transitionImage(cmd, m_offscreenImage,
                            m_offscreenLayout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_offscreenLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }  // конец одного H+V прохода
    }  // if (blurRadius > 0)

    // Апскейл: offscreen (render res, SHADER_READ) → upscaled (full res, SHADER_READ)
    if (m_upscaler != Upscaler::Off)
        recordUpscale(cmd);

    // Swapchain: UNDEFINED → COLOR_ATTACHMENT
    transitionImage(cmd, m_scImages[m_currentImageIdx],
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Composite + UI pass (остаётся открытым — endFrame его закрывает).
    // Depth не подключаем: composite и ImGui его не используют.
    VkRenderingAttachmentInfo colorAttach{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_scImageViews[m_currentImageIdx],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, m_scExtent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttach,
    };
    vkCmdBeginRendering(cmd, &ri);

    VkViewport viewport{
        .x = 0.f, .y = 0.f,
        .width    = static_cast<float>(m_scExtent.width),
        .height   = static_cast<float>(m_scExtent.height),
        .minDepth = 0.f, .maxDepth = 1.f,
    };
    VkRect2D scissor{{0, 0}, m_scExtent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Composite draw: offscreen (или upscaled) → swapchain
    const VkDescriptorSet compositeSrc = (m_upscaler != Upscaler::Off)
        ? m_blurDescSetUpscaled : m_blurDescSetOffscreen;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_blurPipelineLayout, 0, 1, &compositeSrc, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    // pass остаётся открытым — ImGui дорисовывает в вызывающем коде
}

}  // namespace wsim
