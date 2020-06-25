/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include "vulkan.h"

#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

#define VULKAN_DEVICE_FUNCTION(ret, func, params) \
	typedef ret (MOJOSHADER_VKAPIENTRY *vkfntype_MOJOSHADER_##func) params;
#include "mojoshader_vulkan_device_funcs.h"
#undef VULKAN_DEVICE_FUNCTION

#define VULKAN_INSTANCE_FUNCTION(ret, func, params) \
    typedef ret (MOJOSHADER_VKAPIENTRY *vkfntype_MOJOSHADER_##func) params;
#include "mojoshader_vulkan_instance_funcs.h"
#undef VULKAN_INSTANCE_FUNCTION

/* internal struct defs */
typedef struct MOJOSHADER_vkUniformBuffer MOJOSHADER_vkUniformBuffer;
typedef struct MOJOSHADER_vkShader
{
    VkShaderModule shaderModule;
    const MOJOSHADER_parseData *parseData;
    MOJOSHADER_vkUniformBuffer *ubo;
    uint32 refcount;
} MOJOSHADER_vkShader;

// Error state...
static char error_buffer[1024] = { '\0' };

static void set_error(const char *str)
{
    snprintf(error_buffer, sizeof (error_buffer), "%s", str);
} // set_error

static inline void out_of_memory(void)
{
    set_error("out of memory");
} // out_of_memory

#if SUPPORT_PROFILE_SPIRV
#ifdef MOJOSHADER_EFFECT_SUPPORT

/* Structs */

typedef struct MOJOSHADER_vkBufferWrapper
{
    VkBuffer buffer;
    VkDeviceMemory device_memory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void *persistentMap;
} MOJOSHADER_vkBufferWrapper;

struct MOJOSHADER_vkUniformBuffer
{
    unsigned int bufferSize;
    MOJOSHADER_vkBufferWrapper **internalBuffers;
    unsigned int currentFrame;
    int inUse;
    VkDeviceSize internalBufferSize;
    VkDeviceSize internalOffset;
    VkDeviceSize dynamicOffset;
    VkDeviceSize nextDynamicOffsetIncrement;
};

/* Max entries for each register file type */
#define MAX_REG_FILE_F 8192
#define MAX_REG_FILE_I 2047
#define MAX_REG_FILE_B 2047

typedef struct MOJOSHADER_vkEffect
{
    MOJOSHADER_effect *effect;
    unsigned int num_shaders;
    MOJOSHADER_vkShader *shaders;
    unsigned int *shader_indices;
    unsigned int num_preshaders;
    unsigned int *preshader_indices;
    MOJOSHADER_vkShader *current_vert;
    MOJOSHADER_vkShader *current_frag;
    MOJOSHADER_effectShader *current_vert_raw;
    MOJOSHADER_effectShader *current_frag_raw;
    MOJOSHADER_vkShader *prev_vert;
    MOJOSHADER_vkShader *prev_frag;
} MOJOSHADER_vkEffect;

typedef struct MOJOSHADER_vkContext
{
    VkInstance *instance;
    VkPhysicalDevice *physical_device;
    VkDevice *logical_device;
    PFN_vkGetInstanceProcAddr instance_proc_lookup;
    PFN_vkGetDeviceProcAddr device_proc_lookup;
    uint32_t graphics_queue_family_index;
    unsigned int maxUniformBufferRange;
    unsigned int minUniformBufferOffsetAlignment;

    int frames_in_flight;

    MOJOSHADER_malloc malloc_fn;
    MOJOSHADER_free free_fn;
    void *malloc_data;

    /* FIXME: these are freaking huge */
    float vs_reg_file_f[MAX_REG_FILE_F * 4];
    int vs_reg_file_i[MAX_REG_FILE_I * 4];
    uint8_t vs_reg_file_b[MAX_REG_FILE_B * 4];
    float ps_reg_file_f[MAX_REG_FILE_F * 4];
    int ps_reg_file_i[MAX_REG_FILE_I * 4];
    uint8_t ps_reg_file_b[MAX_REG_FILE_B * 4];

    MOJOSHADER_vkUniformBuffer **buffersInUse;
    int bufferArrayCapacity;
    int buffersInUseCount;

    MOJOSHADER_vkBufferWrapper **bufferWrappersToDestroy;
    int bufferWrappersToDestroyCapacity;
    int bufferWrappersToDestroyCount;

    MOJOSHADER_vkShader *vertexShader;
    MOJOSHADER_vkShader *pixelShader;

    #define VULKAN_DEVICE_FUNCTION(ret, func, params) \
        vkfntype_MOJOSHADER_##func func;
    #include "mojoshader_vulkan_device_funcs.h"
    #undef VULKAN_DEVICE_FUNCTION

    #define VULKAN_INSTANCE_FUNCTION(ret, func, params) \
        vkfntype_MOJOSHADER_##func func;
    #include "mojoshader_vulkan_instance_funcs.h"
    #undef VULKAN_INSTANCE_FUNCTION
} MOJOSHADER_vkContext;

static MOJOSHADER_vkContext *ctx = NULL;

/* UBO funcs */

static void queue_free_buffer_wrapper(MOJOSHADER_vkBufferWrapper *buffer)
{
    if (ctx->bufferWrappersToDestroyCount + 1 >= ctx->bufferWrappersToDestroyCapacity)
    {
        int oldCapacity = ctx->bufferWrappersToDestroyCapacity;
        ctx->bufferWrappersToDestroyCapacity *= 2;
        MOJOSHADER_vkBufferWrapper **tmp;
        tmp = (MOJOSHADER_vkBufferWrapper**) ctx->malloc_fn(
            ctx->bufferWrappersToDestroyCapacity * sizeof(MOJOSHADER_vkBufferWrapper*),
            ctx->malloc_data
        );
        memcpy(tmp, ctx->bufferWrappersToDestroy, oldCapacity * sizeof(MOJOSHADER_vkBufferWrapper*));
        ctx->free_fn(ctx->bufferWrappersToDestroy, ctx->malloc_data);
        ctx->bufferWrappersToDestroy = tmp;
    }

    ctx->bufferWrappersToDestroy[ctx->bufferWrappersToDestroyCount] = buffer;
    ctx->bufferWrappersToDestroyCount++;
}

static void free_buffer_wrapper(MOJOSHADER_vkBufferWrapper *buffer)
{
    ctx->vkUnmapMemory(
        *ctx->logical_device,
        buffer->device_memory
    );

    ctx->vkFreeMemory(
        *ctx->logical_device,
        buffer->device_memory,
        NULL
    );

    ctx->vkDestroyBuffer(
        *ctx->logical_device,
        buffer->buffer,
        NULL
    );

    ctx->free_fn(buffer, ctx->malloc_data);
}

static uint8_t find_memory_type(
	uint32_t typeFilter,
	VkMemoryPropertyFlags properties,
	uint32_t *result
) {
	VkPhysicalDeviceMemoryProperties memoryProperties;
	ctx->vkGetPhysicalDeviceMemoryProperties(*ctx->physical_device, &memoryProperties);

	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			*result = i;
			return 1;
		}
	}

	return 0;
}

static uint32_t next_highest_offset_alignment(
    unsigned int offset
) {
    if (offset == 0) { return 0; }
    return offset + (ctx->minUniformBufferOffsetAlignment - (offset % ctx->minUniformBufferOffsetAlignment));
}

static MOJOSHADER_vkBufferWrapper *create_ubo_backing_buffer(
    MOJOSHADER_vkUniformBuffer *ubo,
    int frame
) {
    VkResult vulkanResult;

    MOJOSHADER_vkBufferWrapper *oldBuffer = ubo->internalBuffers[frame];

    MOJOSHADER_vkBufferWrapper *newBuffer = (MOJOSHADER_vkBufferWrapper *) ctx->malloc_fn(
        sizeof(MOJOSHADER_vkBufferWrapper), ctx->malloc_data
    );
    memset(newBuffer, '\0', sizeof(MOJOSHADER_vkBufferWrapper));

    newBuffer->size = ubo->internalBufferSize;

    VkBufferCreateInfo buffer_create_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };

    buffer_create_info.flags = 0;
    buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.size = ubo->internalBufferSize;
    buffer_create_info.queueFamilyIndexCount = 1;
    buffer_create_info.pQueueFamilyIndices = &ctx->graphics_queue_family_index;

    vulkanResult = ctx->vkCreateBuffer(
        *ctx->logical_device,
        &buffer_create_info,
        NULL,
        &newBuffer->buffer
    );
    
    if (vulkanResult != VK_SUCCESS) {
        set_error("error creating VkBuffer in create_ubo_backing_buffer");
        return NULL;
    }

	VkMemoryRequirements memoryRequirements;
	ctx->vkGetBufferMemoryRequirements(
		*ctx->logical_device,
		newBuffer->buffer,
		&memoryRequirements
	);

    VkMemoryAllocateInfo allocate_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };

    if (
        !find_memory_type(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &allocate_info.memoryTypeIndex
        )
    ) {
        set_error("failed to find suitable memory type in create_ubo_backing_buffer");
        return NULL;
    }

    allocate_info.allocationSize = ubo->internalBufferSize;

    vulkanResult = ctx->vkAllocateMemory(
        *ctx->logical_device,
        &allocate_info,
        NULL,
        &newBuffer->device_memory
    );

    if (vulkanResult != VK_SUCCESS)
    {
        set_error("failed to allocate memory for ubo backing buffer");
        return NULL;
    }

    vulkanResult = ctx->vkBindBufferMemory(
        *ctx->logical_device,
        newBuffer->buffer,
        newBuffer->device_memory,
        newBuffer->offset
    );

    if (vulkanResult != VK_SUCCESS)
    {
        set_error("failed to bind ubo backing buffer memory");
        return NULL;
    }

    vulkanResult = ctx->vkMapMemory(
        *ctx->logical_device,
        newBuffer->device_memory,
        0,
        newBuffer->size,
        0,
        &newBuffer->persistentMap
    );

    if (vulkanResult != VK_SUCCESS)
    {
        set_error("error mapping memory in create_ubo_backing_buffer");
        return NULL;
    }

    if (oldBuffer != NULL)
    {
        memcpy(
            newBuffer->persistentMap,
            oldBuffer->persistentMap,
            oldBuffer->size
        );

        queue_free_buffer_wrapper(oldBuffer);
    } // if

    return newBuffer;
} // create_ubo_backing_buffer

static MOJOSHADER_vkUniformBuffer *create_ubo(
    MOJOSHADER_vkShader *shader,
    MOJOSHADER_malloc m, void* d
) {
    int uniformCount = shader->parseData->uniform_count;
    if (uniformCount == 0)
    {
        return NULL;
    }

    /* how big is the buffer? */
    int buflen = 0;
    for (int i = 0; i < uniformCount; i++)
    {
        int arrayCount = shader->parseData->uniforms[i].array_count;
        int uniformSize = 16;
        if (shader->parseData->uniforms[i].type == MOJOSHADER_UNIFORM_BOOL)
        {
            uniformSize = 1;
        }
        buflen += (arrayCount ? arrayCount : 1) * uniformSize;
    } // for 

    MOJOSHADER_vkUniformBuffer *buffer;
    buffer = (MOJOSHADER_vkUniformBuffer*) m(sizeof(MOJOSHADER_vkUniformBuffer), d);
    buffer->bufferSize = ctx->maxUniformBufferRange;
    buffer->internalBufferSize = ((uint64_t)buffer->bufferSize) * 4;
    buffer->internalBuffers = (MOJOSHADER_vkBufferWrapper **) m(
        ctx->frames_in_flight * sizeof(void*), d
    );
    buffer->internalOffset = 0;
    buffer->dynamicOffset = 0;
    buffer->nextDynamicOffsetIncrement = 0;
    buffer->inUse = 0;
    buffer->currentFrame = 0;

    for (int i = 0; i < ctx->frames_in_flight; i++)
    {
        buffer->internalBuffers[i] = NULL;
        buffer->internalBuffers[i] = create_ubo_backing_buffer(buffer, i);
    } // for

    /* could the buffers have different alignment requirements somehow? */

    return buffer;
} // create_ubo

static void *get_uniform_buffer(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return NULL;
    }

    return shader->ubo->internalBuffers[shader->ubo->currentFrame];
} // get_uniform_buffer

static VkDeviceSize get_uniform_offset(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return 0;
    }

    return shader->ubo->internalOffset;
} // get_uniform_offset

static VkDeviceSize get_uniform_dynamic_offset(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return 0;
    }

    return shader->ubo->dynamicOffset;
}

static VkDeviceSize get_uniform_size(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return 0;
    }

    return shader->ubo->bufferSize;
}

static void update_uniform_buffer(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return;
    }

    float *regF; int *regI; uint8_t *regB;
    MOJOSHADER_vkUniformBuffer *ubo = shader->ubo;
    
    if (shader->parseData->shader_type == MOJOSHADER_TYPE_VERTEX)
    {
        regF = ctx->vs_reg_file_f;
        regI = ctx->vs_reg_file_i;
        regB = ctx->vs_reg_file_b;
    }
    else
    {
        regF = ctx->ps_reg_file_f;
        regI = ctx->ps_reg_file_i;
        regB = ctx->ps_reg_file_b;
    }

    if (!ubo->inUse)
    {
        ubo->inUse = 1;
        ctx->buffersInUse[ctx->buffersInUseCount++] = ubo;

        /* allocate more memory if needed */
        if (ctx->buffersInUseCount >= ctx->bufferArrayCapacity)
        {
            int oldlen = ctx->bufferArrayCapacity;
            ctx->bufferArrayCapacity *= 2;
            MOJOSHADER_vkUniformBuffer **tmp;
            tmp = (MOJOSHADER_vkUniformBuffer**) ctx->malloc_fn(
                ctx->bufferArrayCapacity * sizeof(MOJOSHADER_vkUniformBuffer*),
                ctx->malloc_data
            );
            memcpy(tmp, ctx->buffersInUse, oldlen * sizeof(MOJOSHADER_vkUniformBuffer*));
            ctx->free_fn(ctx->buffersInUse, ctx->malloc_data);
            ctx->buffersInUse = tmp;
        }
    }
    else
    {
        ubo->dynamicOffset += next_highest_offset_alignment(ubo->nextDynamicOffsetIncrement);

        if (ubo->dynamicOffset >= ubo->bufferSize)
        {
            ubo->internalOffset += ubo->bufferSize;
            ubo->dynamicOffset = 0;

            /* allocate more memory if needed */
            if (ubo->internalOffset >= ubo->internalBufferSize)
            {
                ubo->internalBufferSize *= 2;
            }

            ubo->internalBuffers[ubo->currentFrame] =
                create_ubo_backing_buffer(ubo, ubo->currentFrame);
        }
    }

    MOJOSHADER_vkBufferWrapper *buf = shader->ubo->internalBuffers[ubo->currentFrame];
    uint8_t *contents = ((uint8_t*)buf->persistentMap) + ubo->dynamicOffset;

    ubo->nextDynamicOffsetIncrement = 0;
    int offset = 0;
    for (int i = 0; i < shader->parseData->uniform_count; i++)
    {
        int index = shader->parseData->uniforms[i].index;
        int arrayCount = shader->parseData->uniforms[i].array_count;
        int size = arrayCount ? arrayCount : 1;

        switch (shader->parseData->uniforms[i].type)
        {
            case MOJOSHADER_UNIFORM_FLOAT:
                memcpy(
                    contents + (offset * 16),
                    &regF[4 * index],
                    size * 16
                );
                ubo->nextDynamicOffsetIncrement += size * 16;
                break;

            case MOJOSHADER_UNIFORM_INT:
                memcpy(
                    contents + (offset * 16),
                    &regI[4 * index],
                    size * 16
                );
                ubo->nextDynamicOffsetIncrement += size * 16;
                break;

            case MOJOSHADER_UNIFORM_BOOL:
                memcpy(
                    contents + offset,
                    &regB[index],
                    size
                );
                ubo->nextDynamicOffsetIncrement += size;
                break;

            default:
                set_error(
                    "SOMETHING VERY WRONG HAPPENED WHEN UPDATING UNIFORMS"
                );
                assert(0);
                break;
        } // switch

        offset += size;
    } // for
} // update_uniform_buffer

static void dealloc_ubo(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL) { return; }

    for (int i = 0; i < ctx->frames_in_flight; i++)
    {
        queue_free_buffer_wrapper(shader->ubo->internalBuffers[i]);
        shader->ubo->internalBuffers[i] = NULL;
    }

    ctx->free_fn(shader->ubo->internalBuffers, ctx->malloc_data);
    ctx->free_fn(shader->ubo, ctx->malloc_data);
} // dealloc_ubo

/* Private funcs */

static void lookup_entry_points(
    MOJOSHADER_vkContext *ctx
) {
    #define VULKAN_DEVICE_FUNCTION(ret, func, params) \
        ctx->func = (vkfntype_MOJOSHADER_##func) ctx->device_proc_lookup(*ctx->logical_device, #func); 
    #include "mojoshader_vulkan_device_funcs.h"
    #undef VULKAN_DEVICE_FUNCTION

    #define VULKAN_INSTANCE_FUNCTION(ret, func, params) \
        ctx->func = (vkfntype_MOJOSHADER_##func) ctx->instance_proc_lookup(*ctx->instance, #func);
    #include "mojoshader_vulkan_instance_funcs.h"
    #undef VULKAN_INSTANCE_FUNCTION
} // lookup_entry_points

static int shader_bytecode_len(MOJOSHADER_vkShader *shader)
{
    return shader->parseData->output_len - sizeof(SpirvPatchTable);
}

static void delete_shader(
    VkShaderModule shaderModule
) {
    ctx->vkDestroyShaderModule(
        *ctx->logical_device,
        shaderModule,
        NULL
    );
}

/* Public API */

MOJOSHADER_vkContext *MOJOSHADER_vkCreateContext(
    MOJOSHADER_VkInstance *instance,
    MOJOSHADER_VkPhysicalDevice *physical_device,
    MOJOSHADER_VkDevice *logical_device,
    int frames_in_flight,
    PFN_MOJOSHADER_vkGetInstanceProcAddr instance_lookup,
    PFN_MOJOSHADER_vkGetDeviceProcAddr device_lookup,
    unsigned int graphics_queue_family_index,
    unsigned int max_uniform_buffer_range,
    unsigned int min_uniform_buffer_offset_alignment,
    MOJOSHADER_malloc m, MOJOSHADER_free f,
    void *malloc_d
) {
    if (m == NULL) m = MOJOSHADER_internal_malloc;
    if (f == NULL) f = MOJOSHADER_internal_free;

    MOJOSHADER_vkContext* resultCtx = (MOJOSHADER_vkContext *) m(sizeof(MOJOSHADER_vkContext), malloc_d);
    if (resultCtx == NULL)
    {
        out_of_memory();
        goto init_fail;
    }

    memset(resultCtx, '\0', sizeof(MOJOSHADER_vkContext));
    resultCtx->malloc_fn = m;
    resultCtx->free_fn = f;
    resultCtx->malloc_data = malloc_d;

    resultCtx->instance = (VkInstance*) instance;
    resultCtx->physical_device = (VkPhysicalDevice*) physical_device;
    resultCtx->logical_device = (VkDevice*) logical_device;
    resultCtx->instance_proc_lookup = (PFN_vkGetInstanceProcAddr) instance_lookup;
    resultCtx->device_proc_lookup = (PFN_vkGetDeviceProcAddr) device_lookup;
    resultCtx->frames_in_flight = frames_in_flight;
    resultCtx->graphics_queue_family_index = graphics_queue_family_index;
    resultCtx->maxUniformBufferRange = max_uniform_buffer_range;
    resultCtx->minUniformBufferOffsetAlignment = min_uniform_buffer_offset_alignment;

    lookup_entry_points(resultCtx);

    resultCtx->bufferArrayCapacity = 32;
    resultCtx->buffersInUseCount = 0;
    resultCtx->buffersInUse = (MOJOSHADER_vkUniformBuffer**) resultCtx->malloc_fn(
        resultCtx->bufferArrayCapacity * sizeof(MOJOSHADER_vkUniformBuffer*),
        resultCtx->malloc_data
    );

    resultCtx->bufferWrappersToDestroyCapacity = 16;
    resultCtx->bufferWrappersToDestroyCount = 0;
    resultCtx->bufferWrappersToDestroy = (MOJOSHADER_vkBufferWrapper**) resultCtx->malloc_fn(
        resultCtx->bufferWrappersToDestroyCapacity * sizeof(MOJOSHADER_vkBufferWrapper*),
        resultCtx->malloc_data
    );

    return resultCtx;

init_fail:
    if (resultCtx != NULL)
    {
        f(resultCtx, malloc_d);
    }
    return NULL;
} // MOJOSHADER_vkCreateContext

void MOJOSHADER_vkMakeContextCurrent(MOJOSHADER_vkContext *_ctx)
{
    ctx = _ctx;
} // MOJOSHADER_vkMakeContextCurrent

void MOJOSHADER_vkDestroyContext()
{
    ctx->free_fn(ctx->buffersInUse, ctx->malloc_data);
    ctx->free_fn(ctx, ctx->malloc_data);
} // MOJOSHADER_vkDestroyContext

MOJOSHADER_vkShader *MOJOSHADER_vkCompileShader(
    const char *mainfn,
    const unsigned char *tokenbuf,
    const unsigned int bufsize,
    const MOJOSHADER_swizzle *swiz,
    const unsigned int swizcount,
    const MOJOSHADER_samplerMap *smap,
    const unsigned int smapcount
) {
    MOJOSHADER_vkShader *shader = NULL;

    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(
        "spirv", mainfn,
        tokenbuf, bufsize,
        swiz, swizcount,
        smap, smapcount,
        ctx->malloc_fn,
        ctx->free_fn,
        ctx->malloc_data
    );

    if (pd->error_count > 0)
    {
        set_error(pd->errors[0].error);
        goto compile_shader_fail;
    }

    shader = (MOJOSHADER_vkShader *) ctx->malloc_fn(sizeof(MOJOSHADER_vkShader), ctx->malloc_data);
    if (shader == NULL)
    {
        out_of_memory();
        goto compile_shader_fail;
    }

    shader->parseData = pd;
    shader->refcount = 1;
    shader->ubo = create_ubo(shader, ctx->malloc_fn, ctx->malloc_data);

    VkShaderModule shaderModule;
    VkShaderModuleCreateInfo shaderModuleCreateInfo = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };

    shaderModuleCreateInfo.flags = 0;
    shaderModuleCreateInfo.codeSize = shader_bytecode_len(shader);
    shaderModuleCreateInfo.pCode = (uint32_t*) pd->output;

    VkResult result = ctx->vkCreateShaderModule(
        *ctx->logical_device,
        &shaderModuleCreateInfo,
        NULL,
        &shader->shaderModule
    );

    if (result != VK_SUCCESS)
    {
        /* FIXME: should display VK error code */
        set_error("Error when creating VkShaderModule");
        goto compile_shader_fail;
    }

    return shader;

compile_shader_fail:
    MOJOSHADER_freeParseData(pd);
    if (shader != NULL)
    {
        delete_shader(shader->shaderModule);
        ctx->free_fn(shader, ctx->malloc_data);
    }
    return NULL;

} // MOJOSHADER_vkMakeContextCurrent

void MOJOSHADER_vkShaderAddRef(MOJOSHADER_vkShader *shader)
{
    if (shader != NULL)
    {
        shader->refcount++;
    }
} // MOJOShader_vkShaderAddRef

void MOJOSHADER_vkDeleteShader(MOJOSHADER_vkShader *shader)
{
    if (shader != NULL)
    {
        if (shader->refcount > 1)
        {
            shader->refcount--;
        }
        else
        {
            dealloc_ubo(shader);
            delete_shader(shader->shaderModule);
            MOJOSHADER_freeParseData(shader->parseData);
            ctx->free_fn(shader, ctx->malloc_data);
        }
    }
} // MOJOSHADER_vkDeleteShader

const MOJOSHADER_parseData *MOJOSHADER_vkGetShaderParseData(
    MOJOSHADER_vkShader *shader
) {
    return (shader != NULL) ? shader->parseData : NULL;
} // MOJOSHADER_vkGetShaderParseData

void MOJOSHADER_vkBindShaders(
    MOJOSHADER_vkShader *vshader,
    MOJOSHADER_vkShader *pshader
) {
    /* NOOP if shader is null */

    if (vshader != NULL)
    {
        ctx->vertexShader = vshader;
    }

    if (pshader != NULL)
    {
        ctx->pixelShader = pshader;
    }
} // MOJOSHADER_vkBindShaders

void MOJOSHADER_vkGetBoundShaders(
    MOJOSHADER_vkShader **vshader,
    MOJOSHADER_vkShader **pshader
) {
    *vshader = ctx->vertexShader;
    *pshader = ctx->pixelShader;
} // MOJOSHADER_vkGetBoundShaders

void MOJOSHADER_vkMapUniformBufferMemory(
    float **vsf, int **vsi, unsigned char **vsb,
    float **psf, int **psi, unsigned char **psb
) {
    *vsf = ctx->vs_reg_file_f;
    *vsi = ctx->vs_reg_file_i;
    *vsb = ctx->vs_reg_file_b;
    *psf = ctx->ps_reg_file_f;
    *psi = ctx->ps_reg_file_i;
    *psb = ctx->ps_reg_file_b;
} // MOJOSHADER_vkMapUniformBufferMemory

void MOJOSHADER_vkUnmapUniformBufferMemory()
{
    /* why is this function named unmap instead of update?
     * the world may never know...
     */

    update_uniform_buffer(ctx->vertexShader);
    update_uniform_buffer(ctx->pixelShader);
} // MOJOSHADER_vkUnmapUniformBufferMemory

void MOJOSHADER_vkGetUniformBuffers(
    void **vbuf, unsigned long long *voff, unsigned long long *vdynamicoff, unsigned long long *vsize,
    void **pbuf, unsigned long long *poff, unsigned long long *pdynamicoff, unsigned long long *psize
) {
    *vbuf = get_uniform_buffer(ctx->vertexShader);
    *voff = get_uniform_offset(ctx->vertexShader);
    *vdynamicoff = get_uniform_dynamic_offset(ctx->vertexShader);
    *vsize = get_uniform_size(ctx->vertexShader);
    *pbuf = get_uniform_buffer(ctx->pixelShader);
    *poff = get_uniform_offset(ctx->pixelShader);
    *pdynamicoff = get_uniform_dynamic_offset(ctx->pixelShader);
    *psize = get_uniform_size(ctx->pixelShader);
} // MOJOSHADER_vkGetUniformBuffers

void MOJOSHADER_vkEndFrame()
{
    for (int i = 0; i < ctx->buffersInUseCount; i++)
    {
        MOJOSHADER_vkUniformBuffer *buf = ctx->buffersInUse[i];
        buf->internalOffset = 0;
        buf->dynamicOffset = 0;
        buf->nextDynamicOffsetIncrement = 0;
        buf->currentFrame = (buf->currentFrame + 1) % ctx->frames_in_flight;
        buf->inUse = 0;
    } // for

    ctx->buffersInUseCount = 0;
} // MOJOSHADER_VkEndFrame

void MOJOSHADER_vkFreeBuffers()
{
    for (int i = 0; i < ctx->bufferWrappersToDestroyCount; i++)
    {
        free_buffer_wrapper(ctx->bufferWrappersToDestroy[i]);
    } // for 

    ctx->bufferWrappersToDestroyCount = 0;
}

int MOJOSHADER_vkGetVertexAttribLocation(MOJOSHADER_vkShader *vert,
                                         MOJOSHADER_usage usage, int index)
{
    if (vert == NULL)
        return -1;

    for (int i = 0; i < vert->parseData->attribute_count; i++)
    {
        if (vert->parseData->attributes[i].usage == usage &&
            vert->parseData->attributes[i].index == index)
        {
            return i;
        } // if
    } // for

    // failure
    return -1;
} //MOJOSHADER_vkGetVertexAttribLocation

unsigned long long MOJOSHADER_vkGetShaderModule(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL) { return 0; }

    return (unsigned long long) shader->shaderModule;
} //MOJOSHADER_vkGetShaderModule

const char *MOJOSHADER_vkGetError(void)
{
    return error_buffer;
} // MOJOSHADER_vkGetError

#endif /* MOJOSHADER_EFFECT_SUPPORT */
#endif /* SUPPORT_PROFILE_SPIRV */

// end of mojoshader_vulkan.c ...
