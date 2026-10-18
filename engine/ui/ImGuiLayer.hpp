#pragma once

#include <string>
#include <vulkan/vulkan.h>

struct GLFWwindow;
struct ImFont;

namespace wsim {

class VulkanContext;

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, VulkanContext& ctx, std::string assetDir);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&)            = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Вызывать до построения UI-команд (ImGui::Begin/Button/...).
    void newFrame();

    // Вызывать после ImGui::Render(), внутри активного dynamic rendering pass.
    void renderDrawData(VkCommandBuffer cmd);

    // Шрифты главного меню (кириллица, Unbounded Bold / Geologica Regular).
    [[nodiscard]] ImFont* titleFont() const noexcept { return m_titleFont; }
    [[nodiscard]] ImFont* uiFont() const noexcept { return m_uiFont; }

private:
    ImFont*          m_titleFont = nullptr;
    ImFont*          m_uiFont    = nullptr;
    VkDevice         m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool   = VK_NULL_HANDLE;
};

}  // namespace wsim
