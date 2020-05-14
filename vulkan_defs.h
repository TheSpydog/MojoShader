/* per Vulkan 1.0 spec */

/* In case this needs to be exported in a certain way... */
#ifndef MOJOSHADER_VKAPIENTRY
#ifdef _WIN32 /* Windows OpenGL uses stdcall */
#define MOJOSHADER_VKAPIENTRY __stdcall
#else
#define MOJOSHADER_VKAPIENTRY
#endif
#endif

#ifndef MOJOSHADER_VK_DEFINE_HANDLE
#define MOJOSHADER_VK_DEFINE_HANDLE(object) typedef struct MOJOSHADER_object##_T* object;
#endif 

#ifndef MOJOSHADER_VKAPI_PTR 
#define MOJOSHADER_VKAPI_PTR
#endif

MOJOSHADER_VK_DEFINE_HANDLE(MOJOSHADER_VkDevice)
MOJOSHADER_VK_DEFINE_HANDLE(MOJOSHADER_VkInstance)
MOJOSHADER_VK_DEFINE_HANDLE(MOJOSHADER_VkPhysicalDevice)

typedef void (MOJOSHADER_VKAPI_PTR *PFN_MOJOSHADER_vkVoidFunction)(void);

typedef PFN_MOJOSHADER_vkVoidFunction (MOJOSHADER_VKAPI_PTR *PFN_MOJOSHADER_vkGetDeviceProcAddr)(
	MOJOSHADER_VkDevice device,
	const char* pName
);
