#include "vkpresent.h"

#include <SDL3/SDL.h>

#define VK_NO_PROTOTYPES
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan_win32.h"
#else
#include "vulkan/vulkan_core.h"
#endif
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ---- OpenGL side: EXT_memory_object(_fd/_win32), EXT_semaphore(_fd/_win32) -----------------
#ifndef GL_TEXTURE_TILING_EXT
#define GL_TEXTURE_TILING_EXT 0x9580
#endif
#ifndef GL_DEDICATED_MEMORY_OBJECT_EXT
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif
#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif
#ifndef GL_HANDLE_TYPE_OPAQUE_WIN32_EXT
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#endif
#ifndef GL_LAYOUT_GENERAL_EXT
#define GL_LAYOUT_GENERAL_EXT 0x958D
#endif
#ifndef GL_DEVICE_UUID_EXT
#define GL_DEVICE_UUID_EXT 0x9597
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
typedef uint64_t GLuint64x;

namespace {

#define PS3VIS_GLX_FUNCS(X)                                                                          \
    X(void, glCreateMemoryObjectsEXT, GLsizei, GLuint *)                                             \
    X(void, glDeleteMemoryObjectsEXT, GLsizei, const GLuint *)                                       \
    X(void, glMemoryObjectParameterivEXT, GLuint, GLenum, const GLint *)                             \
    X(void, glTexStorageMem2DEXT, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64x)      \
    X(void, glGenSemaphoresEXT, GLsizei, GLuint *)                                                   \
    X(void, glDeleteSemaphoresEXT, GLsizei, const GLuint *)                                          \
    X(void, glSignalSemaphoreEXT, GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *) \
    X(void, glWaitSemaphoreEXT, GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *)   \
    X(void, glGetUnsignedBytei_vEXT, GLenum, GLuint, GLubyte *)                                      \
    X(void, glFlush, void)                                                                          \
    X(void, glFinish, void)
#if defined(_WIN32)
#define PS3VIS_GLX_PLATFORM(X)                                                                       \
    X(void, glImportMemoryWin32HandleEXT, GLuint, GLuint64x, GLenum, void *)                         \
    X(void, glImportSemaphoreWin32HandleEXT, GLuint, GLenum, void *)
#else
#define PS3VIS_GLX_PLATFORM(X)                                                                       \
    X(void, glImportMemoryFdEXT, GLuint, GLuint64x, GLenum, GLint)                                   \
    X(void, glImportSemaphoreFdEXT, GLuint, GLenum, GLint)
#endif

#define PS3VIS_GLX_DEFINE(ret, name, ...)                                                            \
    typedef ret(APIENTRY *PFNX_##name)(__VA_ARGS__);                                                 \
    PFNX_##name x_##name = nullptr;
PS3VIS_GLX_FUNCS(PS3VIS_GLX_DEFINE)
PS3VIS_GLX_PLATFORM(PS3VIS_GLX_DEFINE)
#undef PS3VIS_GLX_DEFINE

bool loadGlx(std::string &missing)
{
#define PS3VIS_GLX_LOAD(ret, name, ...)                                                              \
    x_##name = reinterpret_cast<PFNX_##name>(SDL_GL_GetProcAddress(#name));                          \
    if (!x_##name && std::string(#name) != "glGetUnsignedBytei_vEXT")                                \
        missing += std::string(missing.empty() ? "" : ", ") + #name;
    PS3VIS_GLX_FUNCS(PS3VIS_GLX_LOAD)
    PS3VIS_GLX_PLATFORM(PS3VIS_GLX_LOAD)
#undef PS3VIS_GLX_LOAD
    return missing.empty();
}

// Encodes the rendered (sRGB, possibly above 1.0) scene for the swapchain, flipping it
// vertically (GL's first row is the bottom one, Vulkan's the top one).
const char *kEncodeVs = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char *kEncodeFs = R"(#version 330 core
uniform sampler2D scene;
uniform int mode;      // 0 SDR, 1 scRGB, 2 HDR10
uniform float white;   // SDR white, scRGB units
uniform float peak;    // brightest allowed, as a multiple of SDR white
uniform float boost;   // how much brighter than SDR white the clipped highlights get
out vec4 fragColor;
vec3 toLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
void main() {
    ivec2 size = textureSize(scene, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 c = max(texelFetch(scene, ivec2(p.x, size.y - 1 - p.y), 0).rgb, vec3(0.0));
    if (mode == 0) {
        fragColor = vec4(min(c, vec3(1.0)), 1.0);
        return;
    }
    vec3 base = min(c, vec3(1.0)), excess = max(c - 1.0, vec3(0.0));
    vec3 lin = min(toLinear(base) * white + excess * white * boost, vec3(white * peak)); // scRGB
    if (mode == 1) {
        fragColor = vec4(lin, 1.0);
        return;
    }
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
    vec3 y = pow(max(to2020 * lin, vec3(0.0)) * (80.0 / 10000.0), vec3(0.1593017578125));
    fragColor = vec4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375)), 1.0);
}
)";

GLuint compile(const char *vs, const char *fs)
{
    auto stage = [](GLenum type, const char *src) {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HDR encode shader: %s", log);
        }
        return s;
    };
    const GLuint v = stage(GL_VERTEX_SHADER, vs), f = stage(GL_FRAGMENT_SHADER, fs), p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

void checkGl(const char *what)
{
    for (GLenum e; (e = glGetError()) != 0;)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GL error 0x%04x after %s", e, what);
}

#ifndef GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT 0x9588
#endif
#if defined(_WIN32)
constexpr VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
const char *const kExtMem = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
const char *const kExtSem = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
#else
constexpr VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
const char *const kExtMem = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
const char *const kExtSem = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
#endif

} // namespace

// ---- Vulkan side ---------------------------------------------------------------------------
#define PS3VIS_VK_INSTANCE_FUNCS(X)                                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)             \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)               \
    X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                  \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define PS3VIS_VK_DEVICE_FUNCS(X)                                                                    \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)    \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory)          \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)      \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier)    \
    X(vkCmdBlitImage) X(vkQueueSubmit) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)               \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateBuffer)          \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory)          \
    X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer)
#if defined(_WIN32)
#define PS3VIS_VK_PLATFORM_FUNCS(X) X(vkGetMemoryWin32HandleKHR) X(vkGetSemaphoreWin32HandleKHR)
#else
#define PS3VIS_VK_PLATFORM_FUNCS(X) X(vkGetMemoryFdKHR) X(vkGetSemaphoreFdKHR)
#endif

struct VkPresenter::Impl {
    SDL_Window *window = nullptr;
    bool allowHdr = true;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
#define PS3VIS_VK_MEMBER(name) PFN_##name name = nullptr;
    PS3VIS_VK_INSTANCE_FUNCS(PS3VIS_VK_MEMBER)
    PS3VIS_VK_DEVICE_FUNCS(PS3VIS_VK_MEMBER)
    PS3VIS_VK_PLATFORM_FUNCS(PS3VIS_VK_MEMBER)
    PFN_vkSetHdrMetadataEXT vkSetHdrMetadataEXT = nullptr;
#undef PS3VIS_VK_MEMBER

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore glDone = VK_NULL_HANDLE, vkDone = VK_NULL_HANDLE; // shared with GL
    GLuint glGlDone = 0, glVkDone = 0;
    bool hasColorspaceExt = false, hasHdrMetadata = false;
    bool preferHdr = false; // HDR setting "On": use HDR formats if offered, even if SDL says SDR
    bool kmt = false;       // Windows: use KMT memory handles (some GL drivers only import those)
    bool vsync = true;      // false: present immediately (or mailbox), for stress tests

    // Swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered; // one per swapchain image
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    Output output = Output::Sdr;
    bool linearSdr = false; // only an *_SRGB format was offered: write linear values
    float white = 1.0f, peak = 1.0f;

    // Shared image (Vulkan memory, GL texture + FBO), and the GL scene/encode targets.
    VkImage shared = VK_NULL_HANDLE;
    VkDeviceMemory sharedMem = VK_NULL_HANDLE;
    GLuint glMem = 0, glTex = 0, glFbo = 0;
    GLuint sceneTex = 0, sceneFbo = 0;
    int width = 0, height = 0;
    GLuint encodeProg = 0, vao = 0;

    bool dirty = true;
    bool vkDonePending = false;

    // PS3VIS_VK_DEBUG=1: magenta strip drawn by Vulkan at the top of the window, and the centre
    // pixel of the shared image as GL wrote it and as Vulkan reads it, logged every 60 frames.
    bool debug = SDL_getenv("PS3VIS_VK_DEBUG") != nullptr;
    VkBuffer probe = VK_NULL_HANDLE;
    VkDeviceMemory probeMem = VK_NULL_HANDLE;
    void *probeMap = nullptr;
    bool probePending = false;
    long frame = 0;
    long logged = 0; // frames logged step by step (the first few)
    bool trace = false;

    bool fail(std::string &error, const std::string &what)
    {
        error = what;
        return false;
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want)
    {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }
    bool canBlitTo(VkFormat f)
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
        return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT;
    }
    bool windowHdr() const
    {
        return SDL_GetBooleanProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    }

    void destroySwapchainResources()
    {
        for (VkSemaphore s : rendered)
            vkDestroySemaphore(device, s, nullptr);
        rendered.clear();
        images.clear();
    }
    void destroyShared()
    {
        if (glFbo) glDeleteFramebuffers(1, &glFbo);
        if (glTex) glDeleteTextures(1, &glTex);
        if (glMem) x_glDeleteMemoryObjectsEXT(1, &glMem);
        if (sceneFbo) glDeleteFramebuffers(1, &sceneFbo);
        if (sceneTex) glDeleteTextures(1, &sceneTex);
        glFbo = glTex = glMem = sceneFbo = sceneTex = 0;
        if (shared) vkDestroyImage(device, shared, nullptr);
        if (sharedMem) vkFreeMemory(device, sharedMem, nullptr);
        shared = VK_NULL_HANDLE;
        sharedMem = VK_NULL_HANDLE;
        width = height = 0;
    }

    bool createShared(int w, int h)
    {
#if defined(_WIN32)
        const VkExternalMemoryHandleTypeFlagBits memHandle =
            kmt ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT : kMemHandle;
#else
        const VkExternalMemoryHandleTypeFlagBits memHandle = kMemHandle;
#endif
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = memHandle;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = {uint32_t(w), uint32_t(h), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ici, nullptr, &shared) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, shared, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = shared;
        VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exp.pNext = &dedicated;
        exp.handleTypes = memHandle;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &exp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &sharedMem) != VK_SUCCESS ||
            vkBindImageMemory(device, shared, sharedMem, 0) != VK_SUCCESS)
            return false;

        x_glCreateMemoryObjectsEXT(1, &glMem);
        const GLint yes = GL_TRUE;
        x_glMemoryObjectParameterivEXT(glMem, GL_DEDICATED_MEMORY_OBJECT_EXT, &yes);
#if defined(_WIN32)
        VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        gi.memory = sharedMem;
        gi.handleType = memHandle;
        HANDLE handle = nullptr;
        if (vkGetMemoryWin32HandleKHR(device, &gi, &handle) != VK_SUCCESS)
            return false;
        x_glImportMemoryWin32HandleEXT(glMem, req.size,
                                       kmt ? GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT : GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        if (!kmt) // KMT handles are global names, not owned references
            CloseHandle(handle);
#else
        VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gi.memory = sharedMem;
        gi.handleType = kMemHandle;
        int fd = -1;
        if (vkGetMemoryFdKHR(device, &gi, &fd) != VK_SUCCESS)
            return false;
        x_glImportMemoryFdEXT(glMem, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd); // GL owns the fd now
#endif
        checkGl("importing the shared memory");
        glGenTextures(1, &glTex);
        glBindTexture(GL_TEXTURE_2D, glTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        x_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA16F, w, h, glMem, 0);
        checkGl("glTexStorageMem2DEXT");
        glGenFramebuffers(1, &glFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, glFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);
        const bool okShared = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!okShared)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "shared image framebuffer incomplete");
        if (debug && !probe) {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = 256;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(device, &bci, nullptr, &probe);
            VkMemoryRequirements br{};
            vkGetBufferMemoryRequirements(device, probe, &br);
            VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            bai.allocationSize = br.size;
            bai.memoryTypeIndex =
                memoryType(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(device, &bai, nullptr, &probeMem);
            vkBindBufferMemory(device, probe, probeMem, 0);
            vkMapMemory(device, probeMem, 0, VK_WHOLE_SIZE, 0, &probeMap);
        }

        // The scene (what the app renders), floating point so HDR highlights survive.
        glGenTextures(1, &sceneTex);
        glBindTexture(GL_TEXTURE_2D, sceneTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTex, 0);
        const bool okScene = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        width = w;
        height = h;
        return okShared && okScene;
    }

    bool createSwapchain(int w, int h)
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
        extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu)
            extent = {uint32_t(w), uint32_t(h)};
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
        if (extent.width == 0 || extent.height == 0) {
            static int n = 0;
            if (n++ < 5)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: surface extent 0x0 (minimised?)");
            return false;
        }

        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
        auto has = [&](VkFormat f, VkColorSpaceKHR cs) {
            for (auto &sf : formats)
                if (sf.format == f && sf.colorSpace == cs && canBlitTo(f))
                    return true;
            return false;
        };
        SDL_Log("Vulkan surface: extent %ux%u (min %ux%u max %ux%u), images %u..%u, usage 0x%x, alpha 0x%x, transform 0x%x",
                caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height,
                caps.maxImageExtent.width, caps.maxImageExtent.height, caps.minImageCount, caps.maxImageCount,
                caps.supportedUsageFlags, caps.supportedCompositeAlpha, caps.currentTransform);
        for (auto &sf : formats)
            SDL_Log("Vulkan surface format %d colorspace %d%s", int(sf.format), int(sf.colorSpace),
                    canBlitTo(sf.format) ? "" : " (no blit)");
        std::vector<VkPresentModeKHR> modes;
        {
            uint32_t pm = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &pm, nullptr);
            modes.resize(pm);
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &pm, modes.data());
            std::string list;
            for (auto m : modes)
                list += std::to_string(int(m)) + " ";
            SDL_Log("Vulkan present modes: %s; window HDR %d, colorspace ext %d, allow HDR %d", list.c_str(),
                    int(windowHdr()), int(hasColorspaceExt), int(allowHdr));
        }
        VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        format = VK_FORMAT_UNDEFINED;
        linearSdr = false;
        const bool hdr = allowHdr && hasColorspaceExt && (windowHdr() || preferHdr);
        if (hdr && has(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)) {
            format = VK_FORMAT_R16G16B16A16_SFLOAT;
            colorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            output = Output::ScRgb;
        } else if (hdr && (has(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) ||
                           has(VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT))) {
            format = has(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT)
                         ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                         : VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
            output = Output::Hdr10;
        } else {
            output = Output::Sdr;
            for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
                if (format == VK_FORMAT_UNDEFINED && has(f, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR))
                    format = f;
            if (format == VK_FORMAT_UNDEFINED)
                for (VkFormat f : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
                    if (format == VK_FORMAT_UNDEFINED && has(f, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
                        format = f;
                        linearSdr = true; // the format encodes sRGB itself
                    }
        }
        if (format == VK_FORMAT_UNDEFINED) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: no usable swapchain format");
            return false;
        }
        // Brightness scale (from SDL; sensible defaults where the platform doesn't say).
        const SDL_PropertiesID props = SDL_GetWindowProperties(window);
        white = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        peak = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
        if (output != Output::Sdr && !linearSdr) {
            if (white <= 1.0f)
                white = 203.0f / 80.0f; // BT.2408 reference white
            if (peak <= 1.0f)
                peak = 4.0f;
        }

        uint32_t count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = count;
        sci.imageFormat = format;
        sci.imageColorSpace = colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                 ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                 : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, always supported
        if (!vsync) { // unlocked frame rate: immediate, else mailbox
            auto offered = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
            if (offered(VK_PRESENT_MODE_IMMEDIATE_KHR))
                sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (offered(VK_PRESENT_MODE_MAILBOX_KHR))
                sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        const VkResult r = vkCreateSwapchainKHR(device, &sci, nullptr, &next);
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = next;
        if (r != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: vkCreateSwapchainKHR failed (%d)", int(r));
            return false;
        }
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        images.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        rendered.resize(count);
        for (auto &s : rendered) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(device, &si, nullptr, &s);
        }
        if (output == Output::Hdr10 && hasHdrMetadata && vkSetHdrMetadataEXT) {
            VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
            md.displayPrimaryRed = {0.708f, 0.292f};
            md.displayPrimaryGreen = {0.170f, 0.797f};
            md.displayPrimaryBlue = {0.131f, 0.046f};
            md.whitePoint = {0.3127f, 0.3290f};
            md.maxLuminance = white * peak * 80.0f;
            md.minLuminance = 0.0f;
            md.maxContentLightLevel = md.maxLuminance;
            md.maxFrameAverageLightLevel = white * 80.0f;
            vkSetHdrMetadataEXT(device, 1, &swapchain, &md);
        }
        SDL_Log("Vulkan swapchain: format %d colorspace %d, %u images, white %.2f peak %.2f, linearSdr %d", int(format),
                int(colorSpace), count, double(white), double(peak), int(linearSdr));
        SDL_Log("Vulkan swapchain %ux%u, %s", extent.width, extent.height,
                output == Output::Hdr10 ? "HDR10 (PQ)" : output == Output::ScRgb ? "HDR scRGB" : "SDR");
        return true;
    }

    // Vulkan clears the shared image to a known colour; GL must read that colour back.
    bool selfTest()
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = shared;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue test{{0.25f, 0.5f, 0.75f, 1.0f}};
        vkCmdClearColorImage(cmd, shared, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &test, 1, &range);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = queueFamily;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        vkEndCommandBuffer(cmd);
        vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
        vkResetFences(device, 1, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &vkDone;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        x_glWaitSemaphoreEXT(glVkDone, 0, nullptr, 1, &glTex, &layout);
        float px[4] = {};
        glBindFramebuffer(GL_FRAMEBUFFER, glFbo);
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_FLOAT, px);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        checkGl("self-test read");
        const bool ok = std::fabs(px[0] - 0.25f) < 0.01f && std::fabs(px[1] - 0.5f) < 0.01f && std::fabs(px[2] - 0.75f) < 0.01f;
        SDL_Log("Vulkan/GL sharing self-test (%s handles): GL read %.3f %.3f %.3f -> %s", kmt ? "KMT" : "standard",
                double(px[0]), double(px[1]), double(px[2]), ok ? "OK" : "FAILED");
        return ok;
    }

    bool rebuild(int w, int h)
    {
        x_glFinish();
        vkDeviceWaitIdle(device);
        destroySwapchainResources();
        if (w != width || h != height) {
            destroyShared();
            if (!createShared(w, h))
                return false;
        }
        dirty = !createSwapchain(w, h);
        return !dirty;
    }
};

VkPresenter::VkPresenter() : d_(std::make_unique<Impl>()) {}
VkPresenter::~VkPresenter() { shutdown(); }

bool VkPresenter::init(SDL_Window *window, bool allowHdr, std::string &error)
{
    Impl &d = *d_;
    d.window = window;
    d.allowHdr = allowHdr;
    std::string missing;
    if (!loadGlx(missing))
        return d.fail(error, "OpenGL lacks " + missing);

    d.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!d.getInstanceProcAddr)
        return d.fail(error, std::string("no Vulkan loader: ") + SDL_GetError());
    auto gip = d.getInstanceProcAddr;
    auto enumExt = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gip(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gip(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!enumExt || !createInstance)
        return d.fail(error, "Vulkan loader incomplete");

    Uint32 sdlCount = 0;
    const char *const *sdlExt = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    std::vector<const char *> exts(sdlExt, sdlExt + sdlCount);
    uint32_t n = 0;
    enumExt(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    enumExt(nullptr, &n, avail.data());
    for (auto &e : avail)
        if (!std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            d.hasColorspaceExt = true;
        }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PS3 Visualizer";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    {
        std::string list;
        for (auto e : exts)
            list += std::string(e) + " ";
        SDL_Log("Vulkan instance extensions: %s", list.c_str());
    }
    if (createInstance(&ici, nullptr, &d.instance) != VK_SUCCESS)
        return d.fail(error, "cannot create a Vulkan 1.1 instance");
#define PS3VIS_VK_LOAD_I(name)                                                                       \
    d.name = reinterpret_cast<PFN_##name>(gip(d.instance, #name));                                   \
    if (!d.name)                                                                                     \
        return d.fail(error, "Vulkan lacks " #name);
    PS3VIS_VK_INSTANCE_FUNCS(PS3VIS_VK_LOAD_I)
#undef PS3VIS_VK_LOAD_I
    if (!SDL_Vulkan_CreateSurface(window, d.instance, nullptr, &d.surface))
        return d.fail(error, std::string("cannot create a Vulkan surface: ") + SDL_GetError());

    // Use the GPU that OpenGL runs on (matching device UUIDs), with a queue that can present.
    GLubyte glUuid[VK_UUID_SIZE] = {};
    const bool haveUuid = x_glGetUnsignedBytei_vEXT != nullptr;
    if (haveUuid)
        x_glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, glUuid);
    n = 0;
    d.vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    d.vkEnumeratePhysicalDevices(d.instance, &n, gpus.data());
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        d.vkGetPhysicalDeviceProperties2(g, &props);
        if (haveUuid && std::memcmp(id.deviceUUID, glUuid, VK_UUID_SIZE) != 0)
            continue;
        uint32_t qn = 0;
        d.vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        d.vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
        for (uint32_t i = 0; i < qn && !d.gpu; i++) {
            VkBool32 present = VK_FALSE;
            d.vkGetPhysicalDeviceSurfaceSupportKHR(g, i, d.surface, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                d.gpu = g;
                d.queueFamily = i;
                SDL_Log("Vulkan presenter on %s", props.properties.deviceName);
            }
        }
        if (d.gpu)
            break;
    }
    if (!d.gpu)
        return d.fail(error, "no Vulkan GPU matching the OpenGL one can present to the window");

    n = 0;
    d.vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> devAvail(n);
    d.vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &n, devAvail.data());
    auto devHas = [&](const char *name) {
        for (auto &e : devAvail)
            if (!std::strcmp(e.extensionName, name))
                return true;
        return false;
    };
    std::vector<const char *> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, kExtMem, kExtSem};
    for (const char *e : devExts)
        if (!devHas(e))
            return d.fail(error, std::string("Vulkan device lacks ") + e);
    if (devHas(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        devExts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
        d.hasHdrMetadata = true;
    }
    {
        std::string list;
        for (auto e : devExts)
            list += std::string(e) + " ";
        SDL_Log("Vulkan device extensions: %s (queue family %u)", list.c_str(), d.queueFamily);
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    if (d.vkCreateDevice(d.gpu, &dci, nullptr, &d.device) != VK_SUCCESS)
        return d.fail(error, "cannot create the Vulkan device");
#define PS3VIS_VK_LOAD_D(name)                                                                       \
    d.name = reinterpret_cast<PFN_##name>(d.vkGetDeviceProcAddr(d.device, #name));                   \
    if (!d.name)                                                                                     \
        return d.fail(error, "Vulkan lacks " #name);
    PS3VIS_VK_DEVICE_FUNCS(PS3VIS_VK_LOAD_D)
    PS3VIS_VK_PLATFORM_FUNCS(PS3VIS_VK_LOAD_D)
#undef PS3VIS_VK_LOAD_D
    if (d.hasHdrMetadata)
        d.vkSetHdrMetadataEXT =
            reinterpret_cast<PFN_vkSetHdrMetadataEXT>(d.vkGetDeviceProcAddr(d.device, "vkSetHdrMetadataEXT"));
    d.vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.queueFamily;
    d.vkCreateCommandPool(d.device, &pci, nullptr, &d.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    d.vkAllocateCommandBuffers(d.device, &cai, &d.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    d.vkCreateFence(d.device, &fci, nullptr, &d.fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    d.vkCreateSemaphore(d.device, &sci, nullptr, &d.acquired);

    // The two semaphores shared with GL: "GL finished the frame" and "Vulkan finished copying".
    for (auto [vs, gs] : {std::pair{&d.glDone, &d.glGlDone}, std::pair{&d.vkDone, &d.glVkDone}}) {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = kSemHandle;
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        s.pNext = &esci;
        if (d.vkCreateSemaphore(d.device, &s, nullptr, vs) != VK_SUCCESS)
            return d.fail(error, "cannot create a shareable Vulkan semaphore");
        x_glGenSemaphoresEXT(1, gs);
#if defined(_WIN32)
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        gi.semaphore = *vs;
        gi.handleType = kSemHandle;
        HANDLE h = nullptr;
        if (d.vkGetSemaphoreWin32HandleKHR(d.device, &gi, &h) != VK_SUCCESS)
            return d.fail(error, "cannot export a Vulkan semaphore");
        x_glImportSemaphoreWin32HandleEXT(*gs, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h);
        CloseHandle(h);
#else
        VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        gi.semaphore = *vs;
        gi.handleType = kSemHandle;
        int fd = -1;
        if (d.vkGetSemaphoreFdKHR(d.device, &gi, &fd) != VK_SUCCESS)
            return d.fail(error, "cannot export a Vulkan semaphore");
        x_glImportSemaphoreFdEXT(*gs, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
#endif
        checkGl("importing a semaphore");
    }

    // Make sure GL really sees the memory Vulkan writes (on Windows try both handle kinds).
    bool shared = false;
    for (int attempt = 0; attempt < 2 && !shared; attempt++) {
#if defined(_WIN32)
        d.kmt = attempt == 1;
#else
        if (attempt == 1)
            break;
#endif
        shared = d.createShared(64, 64) && d.selfTest();
        d.destroyShared();
    }
    if (!shared)
        return d.fail(error, "OpenGL can't see Vulkan's memory (sharing self-test failed)");

    d.encodeProg = compile(kEncodeVs, kEncodeFs);
    if (!d.encodeProg)
        return d.fail(error, "HDR encode shader failed");
    glGenVertexArrays(1, &d.vao);
    d.dirty = true;
    return true;
}

void VkPresenter::shutdown()
{
    Impl &d = *d_;
    if (d.device) {
        if (d.vkDonePending && x_glWaitSemaphoreEXT)
            x_glWaitSemaphoreEXT(d.glVkDone, 0, nullptr, 0, nullptr, nullptr);
        if (x_glFinish)
            x_glFinish();
        d.vkDeviceWaitIdle(d.device);
        d.destroySwapchainResources();
        d.destroyShared();
        if (d.swapchain)
            d.vkDestroySwapchainKHR(d.device, d.swapchain, nullptr);
        if (d.probe)
            d.vkDestroyBuffer(d.device, d.probe, nullptr);
        if (d.probeMem)
            d.vkFreeMemory(d.device, d.probeMem, nullptr);
        for (GLuint *s : {&d.glGlDone, &d.glVkDone})
            if (*s)
                x_glDeleteSemaphoresEXT(1, s);
        for (VkSemaphore s : {d.glDone, d.vkDone, d.acquired})
            if (s)
                d.vkDestroySemaphore(d.device, s, nullptr);
        if (d.fence)
            d.vkDestroyFence(d.device, d.fence, nullptr);
        if (d.pool)
            d.vkDestroyCommandPool(d.device, d.pool, nullptr);
        d.vkDestroyDevice(d.device, nullptr);
    }
    if (d.encodeProg)
        glDeleteProgram(d.encodeProg);
    if (d.vao)
        glDeleteVertexArrays(1, &d.vao);
    if (d.surface)
        d.vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
    if (d.instance)
        d.vkDestroyInstance(d.instance, nullptr);
    d_ = std::make_unique<Impl>();
}

GLuint VkPresenter::beginFrame(int width, int height)
{
    Impl &d = *d_;
    if (!d.device || width <= 0 || height <= 0)
        return 0;
    // GL may only touch the shared image once Vulkan has finished copying the last frame.
    if (d.vkDonePending) {
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        x_glWaitSemaphoreEXT(d.glVkDone, 0, nullptr, d.glTex ? 1 : 0, d.glTex ? &d.glTex : nullptr, &layout);
        d.vkDonePending = false;
    }
    if (d.dirty || width != d.width || height != d.height)
        if (!d.rebuild(width, height))
            return 0;
    return d.sceneFbo;
}

void VkPresenter::endFrame()
{
    Impl &d = *d_;
    if (!d.device || !d.sceneFbo || d.dirty) {
        static int n = 0;
        if (n++ < 10)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: frame skipped (scene %u, dirty %d)", d.sceneFbo, int(d.dirty));
        return;
    }
    // Encode the scene into the shared image.
    glBindFramebuffer(GL_FRAMEBUFFER, d.glFbo);
    glViewport(0, 0, d.width, d.height);
    glDisable(GL_BLEND);
    glUseProgram(d.encodeProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.sceneTex);
    glUniform1i(glGetUniformLocation(d.encodeProg, "scene"), 0);
    // (an *_SRGB swapchain format encodes by itself: give it linear values, i.e. the scRGB path at 1x)
    const int mode = d.linearSdr ? 1 : d.output == Output::Sdr ? 0 : d.output == Output::ScRgb ? 1 : 2;
    glUniform1i(glGetUniformLocation(d.encodeProg, "mode"), mode);
    glUniform1f(glGetUniformLocation(d.encodeProg, "white"), d.linearSdr ? 1.0f : d.white);
    glUniform1f(glGetUniformLocation(d.encodeProg, "peak"), d.linearSdr ? 1.0f : d.peak);
    glUniform1f(glGetUniformLocation(d.encodeProg, "boost"), d.linearSdr ? 0.0f : boost_);
    glBindVertexArray(d.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    const bool logNow = d.debug && d.frame % 60 == 30;
    if (logNow) {
        float px[4] = {};
        glReadPixels(d.width / 2, d.height / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
        SDL_Log("vk debug frame %ld: GL wrote %.3f %.3f %.3f %.3f at the centre (output %s)", d.frame, double(px[0]),
                double(px[1]), double(px[2]), double(px[3]), describe());
        checkGl("encode pass");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLenum layout = GL_LAYOUT_GENERAL_EXT;
    x_glSignalSemaphoreEXT(d.glGlDone, 0, nullptr, 1, &d.glTex, &layout);
    x_glFlush();

    const bool steps = d.logged < 5 || (d.trace && d.frame % 120 == 0);
    if (steps) {
        d.logged++;
        checkGl("frame encode/signal");
    }
    const VkResult fr = d.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, 2000000000ull);
    if (fr != VK_SUCCESS)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: fence wait returned %d (GPU stuck?)", int(fr));
    d.vkResetFences(d.device, 1, &d.fence);
    if (d.probePending && d.probeMap) {
        const uint16_t *h = static_cast<const uint16_t *>(d.probeMap);
        SDL_Log("vk debug: Vulkan read %.3f %.3f %.3f %.3f from the shared image centre", double(halfToFloat(h[0])),
                double(halfToFloat(h[1])), double(halfToFloat(h[2])), double(halfToFloat(h[3])));
        d.probePending = false;
    }
    d.frame++;
    uint32_t index = 0;
    const VkResult acq = d.vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX, d.acquired, VK_NULL_HANDLE, &index);
    const bool haveImage = acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR;
    if (acq != VK_SUCCESS) {
        static int n = 0;
        if (n++ < 10)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: acquire returned %d", int(acq));
    }
    if (!haveImage || acq == VK_SUBOPTIMAL_KHR)
        d.dirty = true;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.vkResetCommandBuffer(d.cmd, 0);
    d.vkBeginCommandBuffer(d.cmd, &bi);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier in[2]{};
    // Shared image: take it over from GL (external queue family), for reading.
    in[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    in[0].srcAccessMask = 0;
    in[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    in[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    in[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    in[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    in[0].dstQueueFamilyIndex = d.queueFamily;
    in[0].image = d.shared;
    in[0].subresourceRange = range;
    uint32_t inCount = 1;
    if (haveImage) {
        in[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        in[1].srcAccessMask = 0;
        in[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        in[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        in[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        in[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in[1].image = d.images[index];
        in[1].subresourceRange = range;
        inCount = 2;
    }
    d.vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, inCount, in);
    if (haveImage) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {d.width, d.height, 1};
        blit.dstOffsets[1] = {int32_t(d.extent.width), int32_t(d.extent.height), 1};
        if (d.debug) { // magenta everywhere, then the picture below a strip at the top
            const VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}};
            d.vkCmdClearColorImage(d.cmd, d.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            d.vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                   nullptr, 0, nullptr);
            blit.dstOffsets[0].y = int32_t(d.extent.height / 8);
        }
        d.vkCmdBlitImage(d.cmd, d.shared, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.images[index],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (d.debug && d.probe && d.frame % 60 == 31) { // (frame was already counted up above)
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {d.width / 2, d.height / 2, 0}; // the same memory, so the same pixel as GL's
        bic.imageExtent = {1, 1, 1};
        d.vkCmdCopyImageToBuffer(d.cmd, d.shared, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.probe, 1, &bic);
        d.probePending = true;
    }
    VkImageMemoryBarrier out[2]{};
    // Shared image: hand it back to GL.
    out[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    out[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    out[0].dstAccessMask = 0;
    out[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    out[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    out[0].srcQueueFamilyIndex = d.queueFamily;
    out[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    out[0].image = d.shared;
    out[0].subresourceRange = range;
    uint32_t outCount = 1;
    if (haveImage) {
        out[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        out[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        out[1].dstAccessMask = 0;
        out[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        out[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        out[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out[1].image = d.images[index];
        out[1].subresourceRange = range;
        outCount = 2;
    }
    d.vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                           0, nullptr, outCount, out);
    d.vkEndCommandBuffer(d.cmd);

    // Wait for GL (and the swapchain image), signal GL's next wait (and presentation).
    VkSemaphore waits[2] = {d.glDone, d.acquired};
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore signals[2] = {d.vkDone, haveImage ? d.rendered[index] : VK_NULL_HANDLE};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = haveImage ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d.cmd;
    si.signalSemaphoreCount = haveImage ? 2 : 1;
    si.pSignalSemaphores = signals;
    const VkResult sr = d.vkQueueSubmit(d.queue, 1, &si, d.fence);
    if (steps || sr != VK_SUCCESS)
        SDL_Log("Vulkan frame %ld: acquire %d (image %u), submit %d", d.frame, int(acq), index, int(sr));
    d.vkDonePending = true;
    if (haveImage) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &d.rendered[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &d.swapchain;
        pi.pImageIndices = &index;
        const VkResult pr = d.vkQueuePresentKHR(d.queue, &pi);
        if (steps)
            SDL_Log("Vulkan frame %ld: present %d", d.frame, int(pr));
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
            d.dirty = true;
        if (pr != VK_SUCCESS) {
            static int n = 0;
            if (n++ < 10)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: present returned %d", int(pr));
        }
    }
}

void VkPresenter::refresh()
{
    d_->dirty = true;
    if (d_->trace)
        SDL_Log("trace: Vulkan swapchain refresh requested");
}
void VkPresenter::setTrace(bool on) { d_->trace = on; }
void VkPresenter::setVsync(bool on)
{
    if (d_->vsync != on) {
        d_->vsync = on;
        d_->dirty = true;
    }
}
void VkPresenter::setPreferHdr(bool prefer)
{
    if (d_->preferHdr != prefer) {
        d_->preferHdr = prefer;
        d_->dirty = true;
    }
}
void VkPresenter::setAllowHdr(bool allow)
{
    if (d_->allowHdr != allow) {
        d_->allowHdr = allow;
        d_->dirty = true;
    }
}
VkPresenter::Output VkPresenter::output() const { return d_->output; }
float VkPresenter::sdrWhite() const { return d_->white; }
float VkPresenter::headroom() const { return d_->peak; }
bool VkPresenter::offersHdr() const
{
    Impl &d = *d_;
    if (!d.surface || !d.gpu || !d.hasColorspaceExt)
        return false;
    uint32_t n = 0;
    d.vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    d.vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &n, formats.data());
    for (auto &f : formats)
        if ((f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && f.format == VK_FORMAT_R16G16B16A16_SFLOAT) ||
            (f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
             (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)))
            if (d.canBlitTo(f.format))
                return true;
    return false;
}

const char *VkPresenter::describe() const
{
    if (!d_->device)
        return "off";
    switch (d_->output) {
    case Output::Hdr10: return "HDR10";
    case Output::ScRgb: return d_->linearSdr ? "SDR" : "HDR (scRGB)";
    default: return "SDR";
    }
}
