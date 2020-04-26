/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

VULKAN_DEVICE_FUNCTION(BaseVK, VkResult, vkAllocateMemory, (VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo, const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory))
VULKAN_DEVICE_FUNCTION(BaseVK, VkResult, vkBindBufferMemory, (VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset))
VULKAN_DEVICE_FUNCTION(BaseVK, VkResult, vkCreateBuffer, (VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer))
VULKAN_DEVICE_FUNCTION(BaseVK, VkResult, vkCreateShaderModule, (VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule))
VULKAN_DEVICE_FUNCTION(BaseVK, void, vkDestroyBuffer, (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator))
VULKAN_DEVICE_FUNCTION(BaseVK, void, vkDestroyShaderModule, (VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator))
VULKAN_DEVICE_FUNCTION(BaseVK, void, vkFreeMemory, (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator))
VULKAN_DEVICE_FUNCTION(BaseVK, void, vkGetBufferMemoryRequirements, (VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements))
VULKAN_DEVICE_FUNCTION(BaseVK, VkResult, vkMapMemory, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **ppData))
