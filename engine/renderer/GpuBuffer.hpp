#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace wsim {

// Обёртка над VkBuffer + VkDeviceMemory.
// Ручное управление памятью (без VMA) — достаточно для текущего масштаба.
struct GpuBuffer {
    VkBuffer       handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;

    // Создать device-local буфер через staging upload (один submit + wait).
    // Для нескольких буферов сразу используйте UploadBatch — один submit на всё.
    [[nodiscard]] static GpuBuffer createDeviceLocal(
        VkPhysicalDevice physDevice,
        VkDevice         device,
        VkCommandPool    cmdPool,
        VkQueue          queue,
        const void*      data,
        VkDeviceSize     size,
        VkBufferUsageFlags usage
    );

    // Создать host-visible + coherent буфер (для UBO, persistently mapped).
    [[nodiscard]] static GpuBuffer createUniform(
        VkPhysicalDevice physDevice,
        VkDevice         device,
        VkDeviceSize     size
    );

    // Device-local буфер без начальных данных (AS storage, scratch).
    [[nodiscard]] static GpuBuffer createDeviceLocalEmpty(
        VkPhysicalDevice   physDevice,
        VkDevice           device,
        VkDeviceSize       size,
        VkBufferUsageFlags usage
    );

    // Host-visible буфер с произвольным usage (TLAS instance data).
    [[nodiscard]] static GpuBuffer createHostVisible(
        VkPhysicalDevice   physDevice,
        VkDevice           device,
        VkDeviceSize       size,
        VkBufferUsageFlags usage
    );

    [[nodiscard]] void* map(VkDevice device) const;
    void                unmap(VkDevice device) const;
    void                destroy(VkDevice device);

    // Требует usage с VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    [[nodiscard]] VkDeviceAddress address(VkDevice device) const;
};

// Батч загрузки device-local буферов: все copy-команды пишутся в один
// command buffer и уходят одним submit + wait (вместо стойла очереди на буфер).
class UploadBatch {
public:
    UploadBatch(VkPhysicalDevice physDevice, VkDevice device,
                VkCommandPool cmdPool, VkQueue queue);
    ~UploadBatch();  // добивает submit(), если его забыли вызвать

    UploadBatch(const UploadBatch&)            = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    // Записывает copy staging → device-local. Буфер готов после submit().
    [[nodiscard]] GpuBuffer add(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

    // Отправляет все копии одним submit и ждёт завершения.
    void submit();

private:
    VkPhysicalDevice m_physDevice;
    VkDevice         m_device;
    VkCommandPool    m_cmdPool;
    VkQueue          m_queue;
    VkCommandBuffer  m_cmd = VK_NULL_HANDLE;

    std::vector<GpuBuffer> m_stagings;
};

}  // namespace wsim
