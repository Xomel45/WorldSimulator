#pragma once

#include "renderer/Camera.hpp"
#include "renderer/GpuBuffer.hpp"
#include "renderer/Lights.hpp"
#include "renderer/PathTracer.hpp"
#include "renderer/RtSystem.hpp"
#include "renderer/Shadows.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wsim {

class Window;

// ─── Queue family indices ─────────────────────────────────────────────────────
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> compute;
    std::optional<uint32_t> present;
    std::optional<uint32_t> transfer;

    [[nodiscard]] bool isComplete() const noexcept {
        return graphics.has_value() && compute.has_value() && present.has_value();
    }
};

// ─── Swapchain query result ───────────────────────────────────────────────────
struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR        capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

// ─── VulkanContext ─────────────────────────────────────────────────────────────
inline constexpr int kShadowCascadesCount = kShadowCascades;

class VulkanContext {
public:
    VulkanContext(Window& window, bool vsync, std::string shaderDir);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // ── Frame interface ───────────────────────────────────────────────────────
    void beginFrame();
    // Вызывать после всех drawMesh: закрывает scene pass, блюрит фон,
    // открывает UI pass на swapchain. ImGui рисует в этот же pass.
    // blurStrength: 0.0 = без блюра, 1.0 = полный блюр (12px radius).
    void beginUIPass(float blurStrength = 0.f);
    void endFrame();

    // Обновить UBO камеры текущего кадра.
    // Вызывать ПОСЛЕ beginFrame: fence уже дождался GPU, писать в буфер безопасно.
    void updateCamera(const CameraData& cam);

    // Обновить UBO источников света текущего кадра (тоже после beginFrame).
    void updateLights(const LightsData& lights);

    // Переключить vsync: swapchain пересоздастся на следующем beginFrame.
    void setVsync(bool vsync);
    [[nodiscard]] bool vsyncEnabled() const noexcept { return m_vsync; }

    // Сглаживание сцены. Смена применяется на следующем beginFrame
    // (пересоздаются таргеты и scene pipeline).
    void setMsaa(VkSampleCountFlagBits samples);
    [[nodiscard]] VkSampleCountFlagBits msaaSamples() const noexcept { return m_msaa; }
    [[nodiscard]] VkSampleCountFlagBits maxMsaaSamples() const noexcept { return m_maxMsaa; }

    // ── Апскейлинг (AMD FSR 1.0 / NVIDIA Image Scaling) ───────────────────────
    enum class Upscaler { Off, FSR1, NIS };

    // mode + масштаб рендера (0.5..1.0). Применяется на следующем beginFrame.
    void setUpscaler(Upscaler mode, float renderScale);
    void setSharpness(float sharpness);  // 0..1, применяется сразу
    [[nodiscard]] Upscaler upscalerMode() const noexcept { return m_upscaler; }
    [[nodiscard]] float    renderScale() const noexcept { return m_renderScale; }
    [[nodiscard]] float    sharpness() const noexcept { return m_sharpness; }

    // ── Ray tracing (VK_KHR_ray_query) — детали в RtSystem ────────────────────
    [[nodiscard]] bool rayQuerySupported() const noexcept { return m_rt.supported(); }
    [[nodiscard]] bool rayTracedShadows() const noexcept { return m_rtShadows; }
    void setRayTracedShadows(bool enabled) noexcept {
        m_rtShadows = enabled && m_rt.ready();
    }

    // Доп. usage-флаги для vbo/ibo мешей (device address + AS build input при RT).
    [[nodiscard]] VkBufferUsageFlags meshBufferUsage() const noexcept {
        return m_rt.supported()
            ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            : 0;
    }

    // Построить BLAS для мешей и TLAS сцены (вызывать один раз после загрузки).
    using RtInstance = RtSystem::Instance;
    void buildAccelerationStructures(const std::vector<RtInstance>& instances);

    // Трансформы RT-инстансов текущего кадра (в порядке build). Вызывать ДО
    // beginFrame — TLAS текущего кадра перестроится в нём (динамика двигает
    // объекты). Порядок = все mesh-объекты сцены, как при buildAccelerationStructures.
    void setDynamicTransforms(std::vector<glm::mat4> transforms) {
        m_dynamicTransforms = std::move(transforms);
    }

    // ── Path-traced режим (полноценный RT-пайплайн: raygen/miss/chit + SBT) ────
    [[nodiscard]] bool pathTracingSupported() const noexcept { return m_pt.ready(); }
    [[nodiscard]] bool pathTraced() const noexcept { return m_pathTraced; }
    void setPathTraced(bool enabled) noexcept { m_pathTraced = enabled && m_pt.ready(); }

    // ── Симуляция зрения (depth-based defocus) ────────────────────────────────
    // Близорукость: размывается даль за 1/D метров. Дальнозоркость: размывается
    // близь ближе 1/D метров. 0 диоптрий по обеим осям = идеальное зрение (проход
    // выключен). Не работает в path-traced режиме (нет viewDist).
    void setMyopia(float diopters);
    void setHyperopia(float diopters);
    [[nodiscard]] float myopia() const noexcept { return m_myopiaD; }
    [[nodiscard]] float hyperopia() const noexcept { return m_hyperopiaD; }
    [[nodiscard]] bool  visionActive() const noexcept { return m_visionStrength > 0.f; }

    // Push constants для одного draw call (128 байт — гарантированный минимум Vulkan).
    struct MeshPC {
        glm::mat4 model;
        glm::mat4 normalMatrix;  // transpose(inverse(model)), precomputed CPU-side
    };
    static_assert(sizeof(MeshPC) == 128);

    // Нарисовать индексированный меш (вызывать между beginFrame/endFrame)
    void drawMesh(VkBuffer vbo, VkBuffer ibo, uint32_t indexCount, const MeshPC& pc);

    [[nodiscard]] bool isFrameValid() const noexcept { return m_frameValid; }

    // ── Тени от солнца (каскадные карты, см. Shadows.hpp) ─────────────────────
    // Порядок кадра: beginFrame -> updateCamera/updateLights -> для каждого каскада
    // beginShadowCascade/drawShadowMesh.../endShadowCascade -> beginScenePass ->
    // drawMesh... -> beginUIPass -> endFrame. Если теней в кадре нет, каскады
    // можно пропустить целиком (в UBO shadowParams.x = 0).
    static constexpr int kShadowMapSize = 2048;
    void beginShadowCascade(int cascade, const glm::mat4& lightViewProj);
    void drawShadowMesh(VkBuffer vbo, VkBuffer ibo, uint32_t indexCount, const glm::mat4& model);
    void endShadowCascade(int cascade);

    // Открывает основной проход сцены (раньше это делал beginFrame).
    void beginScenePass();

    // Отладка: сохранить сцену следующего кадра (до UI) в PPM. Для самопроверки визуала без захвата экрана.
    void requestSceneDump(std::string path) { m_dumpPath = std::move(path); }

    // Отладка: сохранить готовый кадр (с интерфейсом: меню, панели) в PPM -- единственный способ увидеть UI без захвата экрана.
    void requestFrameDump(std::string path) { m_dumpFinalPath = std::move(path); }

    // Цвет фона/неба (линейный перцептивный, как у offscreen: см. tonemapEncode).
    void setClearColor(const glm::vec4& color) noexcept { m_clearColor = color; }

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] VkInstance       instance() const noexcept { return m_instance; }
    [[nodiscard]] VkDevice         device() const noexcept { return m_device; }
    [[nodiscard]] VkPhysicalDevice physDevice() const noexcept { return m_physDevice; }
    [[nodiscard]] VkQueue          graphicsQueue() const noexcept { return m_graphicsQueue; }
    [[nodiscard]] uint32_t         graphicsQueueFamily() const noexcept { return m_queueIdx.graphics.value(); }
    [[nodiscard]] VkQueue          computeQueue() const noexcept { return m_computeQueue; }
    [[nodiscard]] VkCommandPool    commandPool() const noexcept { return m_cmdPool; }
    [[nodiscard]] VkCommandBuffer  currentCommandBuffer() const noexcept { return m_cmdBuffers[m_currentFrame]; }
    [[nodiscard]] VkExtent2D       swapchainExtent() const noexcept { return m_scExtent; }
    [[nodiscard]] VkFormat         swapchainFormat() const noexcept { return m_scFormat; }

    static constexpr uint32_t kMaxFrames     = 2;  // double-buffered
    static constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kDepthFormat     = VK_FORMAT_D32_SFLOAT;
    // Расстояние камера→фрагмент, отдельный attachment для defocus-прохода (зрение)
    static constexpr VkFormat kViewDistFormat  = VK_FORMAT_R16_SFLOAT;

private:
    // ── Инициализация ─────────────────────────────────────────────────────────
    void createInstance();
    void setupDebugMessenger();
    void createSurface(const Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain(uint32_t width, uint32_t height);
    void createImageViews();
    void createDepthResources();
    void createSyncObjects();
    void createCommandPool();
    void createCommandBuffers();
    void createCameraUBOs();
    void createDescriptors();
    void createScenePipeline();
    void createShadowResources();     // карта теней (массив глубины), sampler
    void createShadowPipeline();      // depth-only проход
    void createOffscreenResources();
    void createBlurPipeline();
    void createVisionPipeline();      // defocus-проход (симуляция зрения)
    void writeVisionDescriptors();    // (пере)привязка offscreen/blurTemp/viewDist
    void createPipelineCache();
    void savePipelineCache() const;
    void writeBlurDescriptors();
    void createUpscalePipelines();    // один раз: layout'ы, пайплайны, NIS coef/sampler
    void createUpscaleTargets();      // full-res images (при recreate)
    void destroyUpscaleTargets();
    void writeUpscaleDescriptors();   // после (пере)создания таргетов
    void updateUpscaleConstants();    // FSR con / NIS config UBO
    void recordUpscale(VkCommandBuffer cmd);  // compute-пассы в кадре

    // Пересоздание swapchain и зависимых ресурсов (resize / vsync / OUT_OF_DATE).
    // При свёрнутом окне (0×0) блокируется до восстановления.
    void recreateSwapchain();

    // ── Helpers ───────────────────────────────────────────────────────────────
    [[nodiscard]] bool               isDeviceSuitable(VkPhysicalDevice dev) const;
    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice dev) const;
    [[nodiscard]] SwapchainSupport   querySwapchainSupport(VkPhysicalDevice dev) const;
    [[nodiscard]] VkShaderModule     loadShader(const std::string& path) const;

    // Image layout transition через synchronization2 (core 1.3)
    static void transitionImage(
        VkCommandBuffer    cmd,
        VkImage            image,
        VkImageLayout      from,
        VkImageLayout      to,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT
    );

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void*                                       userData
    );

    // ── Core handles ──────────────────────────────────────────────────────────
    Window*                  m_window         = nullptr;  // для resize / recreation
    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physDevice     = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;

    QueueFamilyIndices m_queueIdx{};
    VkQueue            m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue            m_computeQueue  = VK_NULL_HANDLE;
    VkQueue            m_presentQueue  = VK_NULL_HANDLE;

    // ── Swapchain ─────────────────────────────────────────────────────────────
    VkSwapchainKHR           m_swapchain = VK_NULL_HANDLE;
    VkFormat                 m_scFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_scExtent{};
    std::vector<VkImage>     m_scImages;
    std::vector<VkImageView> m_scImageViews;

    // ── Sync (per-frame-in-flight) ────────────────────────────────────────────
    uint32_t                 m_currentFrame    = 0;
    uint32_t                 m_currentImageIdx = 0;
    bool                     m_frameValid      = false;
    bool                     m_vsync           = true;
    bool                     m_needsRecreate   = false;
    std::vector<VkSemaphore> m_imageAvailable;
    std::vector<VkSemaphore> m_renderFinished;
    std::vector<VkFence>     m_inFlightFences;

    // ── Commands ──────────────────────────────────────────────────────────────
    VkCommandPool                m_cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_cmdBuffers;  // one per frame in flight

    // ── Depth buffer ──────────────────────────────────────────────────────────
    VkImage        m_depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView    m_depthView   = VK_NULL_HANDLE;

    // ── Offscreen color target (scene) + blur temp ────────────────────────────
    VkImage        m_offscreenImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_offscreenMemory = VK_NULL_HANDLE;
    VkImageView    m_offscreenView   = VK_NULL_HANDLE;
    VkImageLayout  m_offscreenLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // ── View-distance target (симуляция зрения: length(camPos - fragPos)) ──────
    // Scene-шейдеры пишут расстояние вторым color attachment. MSAA-таргет (если
    // MSAA включён) резолвится в 1× sampleable-таргет, который читает defocus-проход.
    VkImage        m_viewDistImage      = VK_NULL_HANDLE;  // MSAA (только при m_msaa>1)
    VkDeviceMemory m_viewDistMemory     = VK_NULL_HANDLE;
    VkImageView    m_viewDistView       = VK_NULL_HANDLE;
    VkImageLayout  m_viewDistMsaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage        m_viewDistResolveImage  = VK_NULL_HANDLE;  // 1×, R16F, SAMPLED
    VkDeviceMemory m_viewDistResolveMemory = VK_NULL_HANDLE;
    VkImageView    m_viewDistResolveView   = VK_NULL_HANDLE;
    VkImageLayout  m_viewDistResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // ── Upscale (render scale + FSR/NIS) ──────────────────────────────────────
    Upscaler   m_upscaler    = Upscaler::Off;
    float      m_renderScale = 1.0f;
    float      m_sharpness   = 0.3f;
    VkExtent2D m_renderExtent{};  // = scExtent * renderScale (offscreen/depth/blur)

    // Full-res цепочка: EASU → temp → RCAS → upscaled (NIS пишет сразу в upscaled)
    VkImage        m_upTempImage   = VK_NULL_HANDLE;
    VkDeviceMemory m_upTempMemory  = VK_NULL_HANDLE;
    VkImageView    m_upTempView    = VK_NULL_HANDLE;
    VkImageLayout  m_upTempLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage        m_upscaledImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_upscaledMemory = VK_NULL_HANDLE;
    VkImageView    m_upscaledView   = VK_NULL_HANDLE;
    VkImageLayout  m_upscaledLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // FSR: layout (sampler+storage) + 2 пайплайна, push-константы посчитаны CPU-side
    VkDescriptorSetLayout m_fsrDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_fsrPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_easuPipeline = VK_NULL_HANDLE;
    VkPipeline            m_rcasPipeline = VK_NULL_HANDLE;
    VkDescriptorSet       m_dsEasu = VK_NULL_HANDLE;  // offscreen → upTemp
    VkDescriptorSet       m_dsRcas = VK_NULL_HANDLE;  // upTemp → upscaled
    uint32_t              m_easuCon[4][4]{};
    uint32_t              m_rcasCon[4]{};

    // NIS: свой layout (UBO+sampler+textures+storage), coef-текстуры из SDK
    VkDescriptorSetLayout m_nisDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_nisPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_nisPipeline = VK_NULL_HANDLE;
    VkDescriptorSet       m_dsNis = VK_NULL_HANDLE;   // offscreen → upscaled
    VkSampler             m_nisSampler = VK_NULL_HANDLE;
    GpuBuffer             m_nisConfigUBO;
    VkImage        m_nisCoefScaleImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_nisCoefScaleMemory = VK_NULL_HANDLE;
    VkImageView    m_nisCoefScaleView   = VK_NULL_HANDLE;
    VkImage        m_nisCoefUsmImage    = VK_NULL_HANDLE;
    VkDeviceMemory m_nisCoefUsmMemory   = VK_NULL_HANDLE;
    VkImageView    m_nisCoefUsmView     = VK_NULL_HANDLE;

    VkDescriptorPool m_upscaleDescPool = VK_NULL_HANDLE;
    VkDescriptorSet  m_blurDescSetUpscaled = VK_NULL_HANDLE;  // composite при апскейле

    // ── MSAA (сцена рендерится в MSAA color, resolve → offscreen) ─────────────
    VkSampleCountFlagBits m_msaa            = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits m_pipelineSamples = VK_SAMPLE_COUNT_1_BIT;  // scene pipeline создан под это
    VkSampleCountFlagBits m_maxMsaa         = VK_SAMPLE_COUNT_1_BIT;
    VkImage        m_msaaColorImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_msaaColorMemory = VK_NULL_HANDLE;
    VkImageView    m_msaaColorView   = VK_NULL_HANDLE;
    VkImageLayout  m_msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage        m_blurTempImage   = VK_NULL_HANDLE;
    VkDeviceMemory m_blurTempMemory  = VK_NULL_HANDLE;
    VkImageView    m_blurTempView    = VK_NULL_HANDLE;
    VkImageLayout  m_blurTempLayout  = VK_IMAGE_LAYOUT_UNDEFINED;

    // ── Blur / composite pipeline ─────────────────────────────────────────────
    VkSampler             m_blurSampler          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blurDescSetLayout    = VK_NULL_HANDLE;
    VkDescriptorPool      m_blurDescPool         = VK_NULL_HANDLE;
    VkDescriptorSet       m_blurDescSetOffscreen = VK_NULL_HANDLE;
    VkDescriptorSet       m_blurDescSetTemp      = VK_NULL_HANDLE;

    VkPipelineLayout      m_blurPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_blurPipeline         = VK_NULL_HANDLE;
    VkPipeline            m_compositePipeline    = VK_NULL_HANDLE;

    // ── Vision defocus (симуляция зрения) — 2 прохода одним пайплайном ─────────
    // gather offscreen→blurTemp (реальные параметры), copy-back blurTemp→offscreen
    // (strength=0 → passthrough). Набор дескрипторов: 0=color, 1=viewDist.
    VkDescriptorSetLayout m_visionDescSetLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_visionDescPool       = VK_NULL_HANDLE;
    VkDescriptorSet       m_visionDescOffscreen  = VK_NULL_HANDLE;  // {offscreen, viewDist}
    VkDescriptorSet       m_visionDescTemp       = VK_NULL_HANDLE;  // {blurTemp, viewDist}
    VkPipelineLayout      m_visionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_visionPipeline       = VK_NULL_HANDLE;

    // Параметры зрения. Зона резкости [nearSharp, farSharp] (м); strength — масштаб
    // диоптрий в пиксели радиуса CoC. strength>0 ⇒ проход активен.
    float m_myopiaD        = 0.f;    // диоптрии близорукости (размытие дали)
    float m_hyperopiaD     = 0.f;    // диоптрии дальнозоркости (размытие близи)
    float m_visionNearSharp = 0.f;   // < этого резкости нет (дальнозоркость)
    float m_visionFarSharp  = 0.f;   // > этого резкости нет (близорукость), 0 = ∞
    float m_visionStrength  = 0.f;   // 0 = идеальное зрение

    // ── Camera + Lights UBO (per frame in flight) ─────────────────────────────
    std::vector<GpuBuffer> m_cameraUBOs;
    std::vector<void*>     m_cameraMaps;  // persistently mapped pointers
    std::vector<GpuBuffer> m_lightsUBOs;
    std::vector<void*>     m_lightsMaps;

    // ── Descriptor sets ───────────────────────────────────────────────────────
    VkDescriptorSetLayout        m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool             m_descPool      = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets;     // kMaxFrames sets

    // ── Тени ──────────────────────────────────────────────────────────────────
    VkImage          m_shadowImage   = VK_NULL_HANDLE;   // D32, kShadowCascades слоёв
    VkDeviceMemory   m_shadowMemory  = VK_NULL_HANDLE;
    VkImageView      m_shadowArrayView = VK_NULL_HANDLE; // для сэмплирования
    VkImageView      m_shadowLayerViews[kShadowCascadesCount] = {};  // для рендера по слоям
    VkSampler        m_shadowSampler = VK_NULL_HANDLE;   // сравнивающий (PCF железом)
    VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_shadowPipeline = VK_NULL_HANDLE;
    glm::mat4        m_shadowLightVP{1.0f};              // матрица текущего каскада (push constant)
    glm::vec4        m_clearColor{0.012f, 0.006f, 0.006f, 1.0f};
    std::string      m_dumpPath;       // непусто -- снимок запрошен
    GpuBuffer        m_dumpBuffer;
    bool             m_dumpPending = false;
    std::string      m_dumpFinalPath;  // снимок готового кадра
    GpuBuffer        m_dumpFinalBuffer;
    bool             m_dumpFinalPending = false;

    // ── Scene pipeline (обычный + вариант с RT-тенями) ────────────────────────
    VkPipelineLayout m_scenePipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_scenePipeline       = VK_NULL_HANDLE;
    VkPipeline       m_scenePipelineRT     = VK_NULL_HANDLE;

    // ── Ray tracing (acceleration structures вынесены в RtSystem) ─────────────
    RtSystem m_rt;
    bool     m_rtShadows = false;  // тумблер из настроек
    std::vector<glm::mat4> m_dynamicTransforms;  // трансформы RT-инстансов кадра

    // Полноценный RT-пайплайн (path-traced режим) — capability + свойства для SBT
    bool m_rtPipeline = false;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtPipeProps{};
    PathTracer m_pt;
    bool       m_pathTraced = false;          // тумблер: трассировать сцену целиком
    glm::mat4  m_invViewProj{1.f};            // для реконструкции первичных лучей
    uint32_t   m_ptFrame = 0;                 // decorrelation RNG между кадрами

    // ── Pipeline cache (персистентный, на диске) ──────────────────────────────
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

    // ── Proc addresses ────────────────────────────────────────────────────────
    PFN_vkCreateDebugUtilsMessengerEXT  m_fpCreateDebugMessenger  = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT m_fpDestroyDebugMessenger = nullptr;

    std::string m_shaderDir;
};

}  // namespace wsim
