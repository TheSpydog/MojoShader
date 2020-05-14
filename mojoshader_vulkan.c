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
    void **data;
} MOJOSHADER_vkBufferWrapper;

struct MOJOSHADER_vkUniformBuffer
{
    int bufferSize;
    MOJOSHADER_vkBufferWrapper **internalBuffers;
    VkDeviceSize internalBufferSize;
    VkDeviceSize internalOffset;
    int currentFrame;
    int inUse;
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
    PFN_vkGetDeviceProcAddr device_proc_lookup;
    uint32_t graphics_queue_family_index;

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

    MOJOSHADER_vkShader *vertexShader;
    MOJOSHADER_vkShader *pixelShader;

    #define VULKAN_DEVICE_FUNCTION(ret, func, params) \
        vkfntype_MOJOSHADER_##func func;
    #include "mojoshader_vulkan_device_funcs.h"
    #undef VULKAN_DEVICE_FUNCTION
} MOJOSHADER_vkContext;

static MOJOSHADER_vkContext *ctx = NULL;

/* UBO funcs */

static VkDeviceSize next_highest_alignment(VkBuffer *buffer, int n)
{
    VkMemoryRequirements memory_requirements;
    ctx->vkGetBufferMemoryRequirements(
        *ctx->logical_device,
        *buffer,
        &memory_requirements
    );

    VkDeviceSize align = memory_requirements.alignment;
    return align * ((n + align - 1) / align);
}

static void free_buffer_wrapper(MOJOSHADER_vkBufferWrapper *buffer)
{
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

    ctx->free_fn(buffer->data, ctx->malloc_data);
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

static MOJOSHADER_vkBufferWrapper *create_ubo_backing_buffer(
    MOJOSHADER_vkUniformBuffer *ubo,
    int frame
) {
    VkResult vulkanResult;

    MOJOSHADER_vkBufferWrapper *oldBuffer = ubo->internalBuffers[frame];

    MOJOSHADER_vkBufferWrapper *newBuffer = ctx->malloc_fn(
        sizeof(MOJOSHADER_vkBufferWrapper), ctx->malloc_data
    );

    newBuffer->data = ctx->malloc_fn(
        ubo->internalBufferSize, ctx->malloc_data
    );

    newBuffer->size = ubo->internalBufferSize;
    newBuffer->offset = 0; /* TODO: is this correct? */

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

    allocate_info.allocationSize = ubo->bufferSize;

    vulkanResult = ctx->vkAllocateMemory(
        *ctx->logical_device,
        &allocate_info,
        NULL,
        &newBuffer->device_memory
    );

    /* there is no way to access contents of a VkBuffer directly...
     * we have to wrap the VkBuffer handle and data and copy it
     */
    if (oldBuffer != NULL)
    {
        vulkanResult = ctx->vkMapMemory(
            *ctx->logical_device,
            newBuffer->device_memory,
            oldBuffer->offset,
            oldBuffer->size,
            0,
            oldBuffer->data
        );

        if (vulkanResult != VK_SUCCESS)
        {
            set_error("error mapping memory in create_ubo_backing_buffer");
            return NULL;
        }

        newBuffer->offset = oldBuffer->offset;
        newBuffer->size = oldBuffer->size;

        memcpy(
            newBuffer->data,
            oldBuffer->data,
            oldBuffer->size
        );

        ctx->vkUnmapMemory(
            *ctx->logical_device,
            newBuffer->device_memory
        );

        free_buffer_wrapper(oldBuffer);
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
    buffer = (MOJOSHADER_vkUniformBuffer*) m(sizeof(MOJOSHADER_vkUniformBuffer*), d);
    buffer->internalBufferSize = buffer->bufferSize * 16;
    buffer->internalBuffers = m(ctx->frames_in_flight * sizeof(MOJOSHADER_vkBufferWrapper*), d);
    buffer->internalOffset = 0;
    buffer->inUse = 0;
    buffer->currentFrame = 0;

    for (int i = 0; i < ctx->frames_in_flight; i++)
    {
        buffer->internalBuffers[i] = NULL;
        buffer->internalBuffers[i] = create_ubo_backing_buffer(buffer, i);
    } // for

    /* could the buffers have different alignment requirements somehow? */
    buffer->bufferSize = next_highest_alignment(&buffer->internalBuffers[0]->buffer, buflen);

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

static unsigned long get_uniform_offset(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return 0;
    }

    return shader->ubo->internalOffset;
} // get_uniform_offset

static unsigned long get_uniform_size(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return 0;
    }

    return shader->ubo->internalBufferSize;
}

static void predraw_ubo(MOJOSHADER_vkUniformBuffer *ubo)
{
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
        return;
    } // if

    ubo->internalOffset += ubo->bufferSize;

    VkDeviceSize buflen = ubo->internalBuffers[ubo->currentFrame]->size;

    if (ubo->internalOffset >= buflen)
    {
        /* allocate more memory if needed */
        if (ubo->internalOffset >= ubo->internalBufferSize)
        {
            ubo->internalBufferSize *= 2;
        }

        ubo->internalBuffers[ubo->currentFrame] =
            create_ubo_backing_buffer(ubo, ubo->currentFrame);
    } // if
} // predraw_ubo

static void update_uniform_buffer(MOJOSHADER_vkShader *shader)
{
    if (shader == NULL || shader->ubo == NULL)
    {
        return;
    }

    float *regF; int *regI; uint8_t *regB;
    
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

    predraw_ubo(shader->ubo);
    MOJOSHADER_vkBufferWrapper *buf = shader->ubo->internalBuffers[shader->ubo->currentFrame];
    void *contents = buf->data + shader->ubo->internalOffset;

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
                break;

            case MOJOSHADER_UNIFORM_INT:
                memcpy(
                    contents + (offset * 16),
                    &regI[4 * index],
                    size * 16
                );
                break;

            case MOJOSHADER_UNIFORM_BOOL:
                memcpy(
                    contents + offset,
                    &regB[index],
                    size
                );
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
        free_buffer_wrapper(shader->ubo->internalBuffers[i]);
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
    PFN_MOJOSHADER_vkGetDeviceProcAddr lookup,
    unsigned int graphics_queue_family_index,
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
    resultCtx->device_proc_lookup = (PFN_vkGetDeviceProcAddr) lookup;
    resultCtx->frames_in_flight = frames_in_flight;
    resultCtx->graphics_queue_family_index = graphics_queue_family_index;

    lookup_entry_points(resultCtx);

    resultCtx->bufferArrayCapacity = 32;
    resultCtx->buffersInUse = resultCtx->malloc_fn(
        resultCtx->bufferArrayCapacity * sizeof(MOJOSHADER_vkUniformBuffer*),
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
    *psf = ctx->vs_reg_file_f;
    *psi = ctx->vs_reg_file_i;
    *psb = ctx->vs_reg_file_b;
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
    void **vbuf, unsigned long *voff, unsigned long *vsize,
    void **pbuf, unsigned long *poff, unsigned long *psize
) {
    *vbuf = get_uniform_buffer(ctx->vertexShader);
    *voff = get_uniform_offset(ctx->vertexShader);
    *vsize = get_uniform_size(ctx->vertexShader);
    *pbuf = get_uniform_buffer(ctx->pixelShader);
    *poff = get_uniform_offset(ctx->pixelShader);
    *psize = get_uniform_size(ctx->pixelShader);
} // MOJOSHADER_vkGetUniformBuffers

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

const char *MOJOSHADER_vkGetError(void)
{
    return error_buffer;
} // MOJOSHADER_vkGetError

#endif /* MOJOSHADER_EFFECT_SUPPORT */
#endif /* SUPPORT_PROFILE_SPIRV */

// end of mojoshader_vulkan.c ...
