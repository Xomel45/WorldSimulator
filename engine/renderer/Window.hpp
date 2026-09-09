#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct GLFWwindow;

namespace wsim {

class Window {
public:
    Window(std::string_view title, uint32_t width, uint32_t height);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool shouldClose() const noexcept;
    void               pollEvents() const noexcept;

    [[nodiscard]] GLFWwindow* handle() const noexcept { return m_handle; }
    [[nodiscard]] uint32_t    width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t    height() const noexcept { return m_height; }

    // Был ли resize с момента последнего вызова (сбрасывает флаг).
    [[nodiscard]] bool consumeResized() noexcept {
        const bool r = m_resized;
        m_resized    = false;
        return r;
    }

    // Расширения Vulkan, требуемые GLFW
    [[nodiscard]] static std::vector<const char*> requiredVulkanExtensions();

private:
    static void framebufferSizeCallback(GLFWwindow* win, int w, int h);

    GLFWwindow* m_handle = nullptr;
    uint32_t    m_width;
    uint32_t    m_height;
    bool        m_resized = false;
};

}  // namespace wsim
