#include "core/InputSystem.hpp"

#include <GLFW/glfw3.h>

#include <utility>

namespace wsim {

namespace {

// Диапазоны валидных GLFW-кодов клавиш: glfwGetKey с невалидным кодом
// генерирует GLFW_INVALID_ENUM, поэтому опрашиваем только существующие.
constexpr std::pair<int, int> kKeyRanges[] = {
    {GLFW_KEY_SPACE,        GLFW_KEY_SPACE},
    {GLFW_KEY_APOSTROPHE,   GLFW_KEY_APOSTROPHE},
    {GLFW_KEY_COMMA,        GLFW_KEY_9},
    {GLFW_KEY_SEMICOLON,    GLFW_KEY_SEMICOLON},
    {GLFW_KEY_EQUAL,        GLFW_KEY_EQUAL},
    {GLFW_KEY_A,            GLFW_KEY_RIGHT_BRACKET},
    {GLFW_KEY_GRAVE_ACCENT, GLFW_KEY_GRAVE_ACCENT},
    {GLFW_KEY_WORLD_1,      GLFW_KEY_WORLD_2},
    {GLFW_KEY_ESCAPE,       GLFW_KEY_END},
    {GLFW_KEY_CAPS_LOCK,    GLFW_KEY_PAUSE},
    {GLFW_KEY_F1,           GLFW_KEY_F25},
    {GLFW_KEY_KP_0,         GLFW_KEY_KP_EQUAL},
    {GLFW_KEY_LEFT_SHIFT,   GLFW_KEY_MENU},
};

// Единственный активный экземпляр — колбэк GLFW статический, а window user
// pointer уже занят Window (framebufferSizeCallback). Приложение всегда
// создаёт ровно один InputSystem на один GLFWwindow.
InputSystem* g_activeInstance = nullptr;

}  // namespace

InputSystem::InputSystem(GLFWwindow* window) : m_win(window) {
    // Старт в меню — курсор виден; setCursorCaptured(true) вызывается при входе в игру.
    g_activeInstance = this;
    glfwSetScrollCallback(window, &InputSystem::scrollCallback);
}

InputSystem::~InputSystem() {
    if (g_activeInstance == this) g_activeInstance = nullptr;
}

void InputSystem::update() {
    m_prev = m_curr;
    for (auto [lo, hi] : kKeyRanges)
        for (int k = lo; k <= hi; ++k)
            m_curr[static_cast<size_t>(k)] = glfwGetKey(m_win, k) == GLFW_PRESS;

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(m_win, &x, &y);

    if (m_firstFrame) {
        m_lastX      = x;
        m_lastY      = y;
        m_firstFrame = false;
    }

    m_dx    = static_cast<float>(x - m_lastX);
    m_dy    = static_cast<float>(y - m_lastY);
    m_lastX = x;
    m_lastY = y;

    m_scrollDY        = m_pendingScrollDY;
    m_pendingScrollDY = 0.0f;
}

bool InputSystem::isKeyDown(int glfwKey) const noexcept {
    if (glfwKey < 0 || glfwKey >= kMaxKeys) return false;
    return m_curr[static_cast<size_t>(glfwKey)];
}

bool InputSystem::isMouseButtonDown(int glfwButton) const noexcept {
    return glfwGetMouseButton(m_win, glfwButton) == GLFW_PRESS;
}

void InputSystem::scrollCallback(GLFWwindow*, double /*xoffset*/, double yoffset) {
    if (g_activeInstance) g_activeInstance->m_pendingScrollDY += static_cast<float>(yoffset);
}

bool InputSystem::isKeyPressed(int glfwKey) const noexcept {
    if (glfwKey < 0 || glfwKey >= kMaxKeys) return false;
    return m_curr[static_cast<size_t>(glfwKey)] && !m_prev[static_cast<size_t>(glfwKey)];
}

void InputSystem::setCursorCaptured(bool captured) {
    glfwSetInputMode(m_win, GLFW_CURSOR,
        captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured && glfwRawMouseMotionSupported())
        glfwSetInputMode(m_win, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    // Сбрасываем позицию, чтобы не было прыжка дельты при переходе.
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(m_win, &x, &y);
    m_lastX = x;
    m_lastY = y;
}

}  // namespace wsim
