#include "ui/ImGuiLayer.hpp"

#include "renderer/VulkanContext.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <stdexcept>
#include <string>

namespace wsim {

ImGuiLayer::ImGuiLayer(GLFWwindow* window, VulkanContext& ctx, std::string assetDir)
    : m_device(ctx.device())
{
    // ── Descriptor pool (только для ImGui) ────────────────────────────────────
    VkDescriptorPoolSize poolSize{
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1000,
    };
    VkDescriptorPoolCreateInfo poolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 1000,
        .poolSizeCount = 1,
        .pPoolSizes    = &poolSize,
    };
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS)
        throw std::runtime_error("ImGuiLayer: failed to create descriptor pool");

    // ── ImGui context ─────────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // не трогать курсор GLFW

    // ── Шрифт с кириллицей ────────────────────────────────────────────────────
    const std::string fontPath = assetDir + "fonts/LiberationSans-Regular.ttf";
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        fontPath.c_str(), 18.f, nullptr,
        io.Fonts->GetGlyphRangesCyrillic());
    if (!font)
        io.Fonts->AddFontDefault();  // ASCII fallback если файл не найден

    // ── Horror style ──────────────────────────────────────────────────────────
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowBorderSize = 0.f;
    s.WindowRounding   = 0.f;
    s.FrameRounding    = 2.f;
    s.ItemSpacing      = {8.f, 8.f};

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = {0.f,    0.f,    0.f,    0.75f};
    c[ImGuiCol_Button]           = {0.15f,  0.02f,  0.02f,  1.f};
    c[ImGuiCol_ButtonHovered]    = {0.40f,  0.05f,  0.05f,  1.f};
    c[ImGuiCol_ButtonActive]     = {0.60f,  0.10f,  0.10f,  1.f};
    c[ImGuiCol_Text]             = {0.90f,  0.85f,  0.80f,  1.f};
    c[ImGuiCol_TextDisabled]     = {0.35f,  0.30f,  0.28f,  1.f};

    // ── GLFW backend ─────────────────────────────────────────────────────────
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // ── Vulkan backend (Dynamic Rendering, ImGui 1.92+) ───────────────────────
    VkFormat colorFmt = ctx.swapchainFormat();

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion      = VK_API_VERSION_1_3;
    initInfo.Instance        = ctx.instance();
    initInfo.PhysicalDevice  = ctx.physDevice();
    initInfo.Device          = ctx.device();
    initInfo.QueueFamily     = ctx.graphicsQueueFamily();
    initInfo.Queue           = ctx.graphicsQueue();
    initInfo.DescriptorPool  = m_pool;
    initInfo.MinImageCount   = VulkanContext::kMaxFrames;
    initInfo.ImageCount      = VulkanContext::kMaxFrames;

    // UI pass — только color (depth в нём не подключается)
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &colorFmt,
    };

    ImGui_ImplVulkan_Init(&initInfo);
}

ImGuiLayer::~ImGuiLayer() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(m_device, m_pool, nullptr);
}

void ImGuiLayer::newFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::renderDrawData(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

}  // namespace wsim
