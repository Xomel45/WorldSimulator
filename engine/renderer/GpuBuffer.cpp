#include "renderer/GpuBuffer.hpp"

#include <cstring>
#include <stdexcept>

namespace wsim {

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace {

uint32_t findMemoryType(
    VkPhysicalDevice      physDevice,
    uint32_t              typeFilter,
    VkMemoryPropertyFlags required
) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physDevice, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((typeFilter & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;

    throw std::runtime_error("GpuBuffer: подходящий тип памяти не найден");
}

GpuBuffer createBuffer(
    VkPhysicalDevice      physDevice,
    VkDevice              device,
    VkDeviceSize          size,
    VkBufferUsageFlags    usage,
    VkMemoryPropertyFlags memProps
) {
    GpuBuffer buf;
    buf.size = size;

    VkBufferCreateInfo ci{
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(device, &ci, nullptr, &buf.handle) != VK_SUCCESS)
        throw std::runtime_error("GpuBuffer: vkCreateBuffer failed");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf.handle, &req);

    // SHADER_DEVICE_ADDRESS требует DEVICE_ADDRESS_BIT при аллокации памяти
    VkMemoryAllocateFlagsInfo allocFlags{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const bool needsAddress = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;

    VkMemoryAllocateInfo alloc{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = needsAddress ? &allocFlags : nullptr,
        .allocationSize  = req.size,
        .memoryTypeIndex = findMemoryType(physDevice, req.memoryTypeBits, memProps),
    };
    if (vkAllocateMemory(device, &alloc, nullptr, &buf.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf.handle, nullptr);
        throw std::runtime_error("GpuBuffer: vkAllocateMemory failed");
    }
    vkBindBufferMemory(device, buf.handle, buf.memory, 0);
    return buf;
}

VkCommandBuffer beginOneTime(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo alloc{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void endOneTime(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

}  // namespace

// ─── Public factory methods ───────────────────────────────────────────────────

GpuBuffer GpuBuffer::createDeviceLocal(
    VkPhysicalDevice   physDevice,
    VkDevice           device,
    VkCommandPool      cmdPool,
    VkQueue            queue,
    const void*        data,
    VkDeviceSize       size,
    VkBufferUsageFlags usage
) {
    UploadBatch batch(physDevice, device, cmdPool, queue);
    GpuBuffer   buf = batch.add(data, size, usage);
    batch.submit();
    return buf;
}

GpuBuffer GpuBuffer::createUniform(
    VkPhysicalDevice physDevice,
    VkDevice         device,
    VkDeviceSize     size
) {
    return createBuffer(
        physDevice, device, size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
}

GpuBuffer GpuBuffer::createDeviceLocalEmpty(
    VkPhysicalDevice   physDevice,
    VkDevice           device,
    VkDeviceSize       size,
    VkBufferUsageFlags usage
) {
    return createBuffer(physDevice, device, size, usage,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

GpuBuffer GpuBuffer::createHostVisible(
    VkPhysicalDevice   physDevice,
    VkDevice           device,
    VkDeviceSize       size,
    VkBufferUsageFlags usage
) {
    return createBuffer(
        physDevice, device, size, usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
}

// ─── Map / Unmap / Destroy ────────────────────────────────────────────────────

void* GpuBuffer::map(VkDevice device) const {
    void* ptr = nullptr;
    vkMapMemory(device, memory, 0, size, 0, &ptr);
    return ptr;
}

VkDeviceAddress GpuBuffer::address(VkDevice device) const {
    VkBufferDeviceAddressInfo info{
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = handle,
    };
    return vkGetBufferDeviceAddress(device, &info);
}

void GpuBuffer::unmap(VkDevice device) const {
    vkUnmapMemory(device, memory);
}

void GpuBuffer::destroy(VkDevice device) {
    if (handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
    size = 0;
}

// ─── UploadBatch ──────────────────────────────────────────────────────────────

UploadBatch::UploadBatch(VkPhysicalDevice physDevice, VkDevice device,
                         VkCommandPool cmdPool, VkQueue queue)
    : m_physDevice(physDevice), m_device(device), m_cmdPool(cmdPool), m_queue(queue) {
    m_cmd = beginOneTime(m_device, m_cmdPool);
}

UploadBatch::~UploadBatch() {
    if (m_cmd != VK_NULL_HANDLE) submit();
}

GpuBuffer UploadBatch::add(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    GpuBuffer staging = createBuffer(
        m_physDevice, m_device, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* mapped = staging.map(m_device);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    staging.unmap(m_device);

    GpuBuffer buf = createBuffer(
        m_physDevice, m_device, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    VkBufferCopy region{.size = size};
    vkCmdCopyBuffer(m_cmd, staging.handle, buf.handle, 1, &region);

    m_stagings.push_back(staging);
    return buf;
}

void UploadBatch::submit() {
    if (m_cmd == VK_NULL_HANDLE) return;

    endOneTime(m_device, m_cmdPool, m_queue, m_cmd);
    m_cmd = VK_NULL_HANDLE;

    for (auto& s : m_stagings) s.destroy(m_device);
    m_stagings.clear();
}

}  // namespace wsim
