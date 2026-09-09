#include "renderer/Window.hpp"

#include "core/Logger.hpp"

#include <GLFW/glfw3.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace wsim {

// ─── Процедурная иконка 32×32 RGBA (буква «П» на тёмном фоне) ────────────────
static std::vector<uint8_t> makeIcon() {
    constexpr int S = 32;
    std::vector<uint8_t> px(S * S * 4);

    // Заливаем фон: тёмно-багровый
    for (int i = 0; i < S * S; ++i) {
        px[i * 4 + 0] = 0x0A;  // R
        px[i * 4 + 1] = 0x02;  // G
        px[i * 4 + 2] = 0x02;  // B
        px[i * 4 + 3] = 0xFF;  // A
    }

    auto fill = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int dy = 0; dy < h; ++dy) {
            for (int dx = 0; dx < w; ++dx) {
                int px_ = x + dx, py_ = y + dy;
                if (px_ < 0 || px_ >= S || py_ < 0 || py_ >= S) continue;
                int i = (py_ * S + px_) * 4;
                px[i] = r;  px[i+1] = g;  px[i+2] = b;  px[i+3] = 0xFF;
            }
        }
    };

    // Буква «П»: пепельно-белый на тёмном фоне
    constexpr uint8_t R = 0xB8, G = 0xA8, B = 0xA0;
    fill(4,  4, 24, 5,  R, G, B);   // верхняя перекладина
    fill(4,  4, 6,  24, R, G, B);   // левая стойка
    fill(22, 4, 6,  24, R, G, B);   // правая стойка

    return px;
}

// ─── Constructor ──────────────────────────────────────────────────────────────
Window::Window(std::string_view title, uint32_t width, uint32_t height)
    : m_width(width), m_height(height) {
    if (!glfwInit())
        throw std::runtime_error("glfwInit() failed");

    // Имя приложения для оконного менеджера (X11 WM_CLASS, Wayland app_id)
    glfwWindowHintString(GLFW_X11_CLASS_NAME,    "worldsim");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "worldsim");
    glfwWindowHintString(GLFW_WAYLAND_APP_ID,    "worldsim");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // без OpenGL-контекста
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);

    m_handle = glfwCreateWindow(
        static_cast<int>(width), static_cast<int>(height),
        title.data(), nullptr, nullptr
    );

    if (!m_handle) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow() failed");
    }

    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, framebufferSizeCallback);

    // Иконка окна
    auto iconData = makeIcon();
    GLFWimage icon{ 32, 32, iconData.data() };
    glfwSetWindowIcon(m_handle, 1, &icon);

    Logger::info("Window {}x{} \"{}\"", width, height, title);
}

Window::~Window() {
    if (m_handle) glfwDestroyWindow(m_handle);
    glfwTerminate();
}

void Window::framebufferSizeCallback(GLFWwindow* win, int w, int h) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
    self->m_width   = static_cast<uint32_t>(w);
    self->m_height  = static_cast<uint32_t>(h);
    self->m_resized = true;
}

bool Window::shouldClose() const noexcept {
    return glfwWindowShouldClose(m_handle);
}

void Window::pollEvents() const noexcept {
    glfwPollEvents();
}

std::vector<const char*> Window::requiredVulkanExtensions() {
    uint32_t     count      = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    return {extensions, extensions + count};
}

}  // namespace wsim
