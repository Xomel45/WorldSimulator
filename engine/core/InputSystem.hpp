#pragma once

#include <array>

struct GLFWwindow;

namespace wsim {

class InputSystem {
public:
    explicit InputSystem(GLFWwindow* window);
    ~InputSystem();

    InputSystem(const InputSystem&)            = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // Вызывать один раз в начале каждого кадра (до чтения состояния).
    void update();

    // Клавиша удерживается прямо сейчас (снимок на момент update()).
    [[nodiscard]] bool isKeyDown(int glfwKey) const noexcept;

    // Клавиша нажата именно в этом кадре (edge: up → down).
    [[nodiscard]] bool isKeyPressed(int glfwKey) const noexcept;

    // Дельта мыши (пиксели) с момента предыдущего update().
    [[nodiscard]] float mouseDX() const noexcept { return m_dx; }
    [[nodiscard]] float mouseDY() const noexcept { return m_dy; }

    // Кнопка мыши удерживается прямо сейчас (GLFW_MOUSE_BUTTON_*).
    [[nodiscard]] bool isMouseButtonDown(int glfwButton) const noexcept;

    // Накопленная вертикальная прокрутка колеса с прошлого update() (сбрасывается им).
    [[nodiscard]] float scrollDY() const noexcept { return m_scrollDY; }

    // Переключает GLFW_CURSOR_DISABLED ↔ GLFW_CURSOR_NORMAL.
    // Сбрасывает дельту, чтобы не было прыжка камеры при переходе.
    void setCursorCaptured(bool captured);

private:
    static constexpr int kMaxKeys = 349;  // GLFW_KEY_LAST + 1

    static void scrollCallback(GLFWwindow* win, double xoffset, double yoffset);

    GLFWwindow* m_win;
    double      m_lastX      = 0.0;
    double      m_lastY      = 0.0;
    float       m_dx         = 0.0f;
    float       m_dy         = 0.0f;
    bool        m_firstFrame = true;
    float       m_scrollDY       = 0.0f;  // накапливается коллбэком, читается/сбрасывается в update()
    float       m_pendingScrollDY = 0.0f;

    std::array<bool, kMaxKeys> m_curr{};
    std::array<bool, kMaxKeys> m_prev{};
};

}  // namespace wsim
