//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <RvCommon/GlVkSharedImage.h>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QVulkanInstance>

#include <vulkan/vulkan.h>

#include <unistd.h>
#include <cstring>
#include <iostream>

using namespace std;

// GL_EXT_memory_object / _fd (not always on QOpenGLExtraFunctions)
typedef void(APIENTRY* PFN_glCreateMemoryObjectsEXT)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glDeleteMemoryObjectsEXT)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFN_glImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint);
typedef void(APIENTRY* PFN_glMemoryObjectParameterivEXT)(GLuint, GLenum, const GLint*);
typedef void(APIENTRY* PFN_glTextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void(APIENTRY* PFN_glCreateTextures)(GLenum, GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glCreateFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glNamedFramebufferTexture)(GLuint, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFN_glBlitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                                   GLint, GLbitfield, GLenum);
typedef void(APIENTRY* PFN_glMemoryBarrier)(GLbitfield);

#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif
#ifndef GL_DEDICATED_MEMORY_OBJECT_EXT
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#endif
#ifndef GL_TEXTURE_UPDATE_BARRIER_BIT
#define GL_TEXTURE_UPDATE_BARRIER_BIT 0x00000100
#endif
#ifndef GL_FRAMEBUFFER_BARRIER_BIT
#define GL_FRAMEBUFFER_BARRIER_BIT 0x00000400
#endif
#ifndef GL_TEXTURE_TILING_EXT
#define GL_TEXTURE_TILING_EXT 0x9580
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif

// Qt builds with VK_NO_PROTOTYPES — load every Vulkan entry point we need.
struct GlVkSharedImageNative
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memSize = 0;
    // Separate sample target (not exportable) — QRhi samples this after GPU copy.
    VkImage sampleImage = VK_NULL_HANDLE;
    VkDeviceMemory sampleMemory = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;

    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
};

namespace Rv
{
    namespace
    {
        // Default ON. Set RV_HDR_GL_VK_INTEROP=0 to force CPU readback.
        bool envDisabled()
        {
            const char* e = getenv("RV_HDR_GL_VK_INTEROP");
            if (!e || !*e)
                return false;
            return !strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no");
        }

        template <typename T>
        T glProc(QOpenGLContext* ctx, const char* name)
        {
            return reinterpret_cast<T>(ctx->getProcAddress(name));
        }

        template <typename T>
        T devProc(GlVkSharedImageNative* n, const char* name)
        {
            return reinterpret_cast<T>(n->vkGetDeviceProcAddr(n->device, name));
        }

        template <typename T>
        T instProc(QVulkanInstance* inst, const char* name)
        {
            return reinterpret_cast<T>(inst->getInstanceProcAddr(name));
        }

        uint32_t findMemoryType(GlVkSharedImageNative* n, uint32_t typeBits, VkMemoryPropertyFlags flags)
        {
            VkPhysicalDeviceMemoryProperties mp;
            n->vkGetPhysicalDeviceMemoryProperties(n->physDev, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
                    return i;
            }
            return UINT32_MAX;
        }

        VkImageMemoryBarrier makeBarrier(VkImage image, VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA,
                                         VkAccessFlags dstA)
        {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = srcA;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            return b;
        }
    } // namespace

    GlVkSharedImage::GlVkSharedImage() = default;

    GlVkSharedImage::~GlVkSharedImage() { destroy(); }

    QByteArrayList GlVkSharedImage::requiredDeviceExtensions()
    {
        return {QByteArrayLiteral("VK_KHR_external_memory"), QByteArrayLiteral("VK_KHR_external_memory_fd"),
                QByteArrayLiteral("VK_KHR_dedicated_allocation"), QByteArrayLiteral("VK_KHR_get_memory_requirements2")};
    }

    bool GlVkSharedImage::isSupported(QRhi* rhi, QOpenGLContext* glctx)
    {
        if (envDisabled() || !rhi || !glctx)
            return false;
        if (rhi->backend() != QRhi::Vulkan)
            return false;
        if (!glProc<PFN_glCreateMemoryObjectsEXT>(glctx, "glCreateMemoryObjectsEXT"))
            return false;
        if (!glProc<PFN_glImportMemoryFdEXT>(glctx, "glImportMemoryFdEXT"))
            return false;
        if (!glProc<PFN_glTextureStorageMem2DEXT>(glctx, "glTextureStorageMem2DEXT"))
            return false;
        return true;
    }

    bool GlVkSharedImage::valid() const { return m_vk && m_vk->image && m_vk->sampleImage && m_glTex && m_glFbo && m_sampleTex; }

    void GlVkSharedImage::destroy()
    {
        m_sampleTex.reset();
        destroyGL();
        destroyVulkan();
        m_size = QSize();
        m_float16 = false;
        m_layout = int(VK_IMAGE_LAYOUT_UNDEFINED);
    }

    void GlVkSharedImage::destroyGL()
    {
        if (QOpenGLContext* ctx = QOpenGLContext::currentContext())
        {
            auto delMem = glProc<PFN_glDeleteMemoryObjectsEXT>(ctx, "glDeleteMemoryObjectsEXT");
            auto delTex = glProc<void (*)(GLsizei, const GLuint*)>(ctx, "glDeleteTextures");
            auto delFbo = glProc<void (*)(GLsizei, const GLuint*)>(ctx, "glDeleteFramebuffers");
            if (m_glFbo && delFbo)
                delFbo(1, &m_glFbo);
            if (m_glTex && delTex)
                delTex(1, &m_glTex);
            if (m_glMem && delMem)
                delMem(1, &m_glMem);
        }
        m_glFbo = 0;
        m_glTex = 0;
        m_glMem = 0;
    }

    void GlVkSharedImage::destroyVulkan()
    {
        if (!m_vk)
            return;
        if (m_vk->device)
        {
            if (m_vk->vkDeviceWaitIdle)
                m_vk->vkDeviceWaitIdle(m_vk->device);
            if (m_vk->cmdPool && m_vk->vkDestroyCommandPool)
                m_vk->vkDestroyCommandPool(m_vk->device, m_vk->cmdPool, nullptr);
            m_vk->cmdPool = VK_NULL_HANDLE;
            if (m_vk->sampleImage && m_vk->vkDestroyImage)
                m_vk->vkDestroyImage(m_vk->device, m_vk->sampleImage, nullptr);
            if (m_vk->sampleMemory && m_vk->vkFreeMemory)
                m_vk->vkFreeMemory(m_vk->device, m_vk->sampleMemory, nullptr);
            if (m_vk->image && m_vk->vkDestroyImage)
                m_vk->vkDestroyImage(m_vk->device, m_vk->image, nullptr);
            if (m_vk->memory && m_vk->vkFreeMemory)
                m_vk->vkFreeMemory(m_vk->device, m_vk->memory, nullptr);
        }
        m_vk.reset();
        m_layout = int(VK_IMAGE_LAYOUT_UNDEFINED);
    }

    bool GlVkSharedImage::ensureCommandPool()
    {
        if (!m_vk || !m_vk->device)
            return false;
        if (m_vk->cmdPool)
            return true;
        if (!m_vk->vkCreateCommandPool)
            return false;

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_vk->queueFamily;
        if (m_vk->vkCreateCommandPool(m_vk->device, &pci, nullptr, &m_vk->cmdPool) != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: vkCreateCommandPool failed" << endl;
            m_vk->cmdPool = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool GlVkSharedImage::transitionImage(int oldLayout, int newLayout, uint32_t srcAccess, uint32_t dstAccess,
                                          uint32_t srcStage, uint32_t dstStage)
    {
        if (!m_vk || !m_vk->image || !m_vk->queue || !ensureCommandPool())
            return false;
        if (oldLayout == newLayout)
            return true;

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_vk->cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (m_vk->vkAllocateCommandBuffers(m_vk->device, &ai, &cmd) != VK_SUCCESS)
            return false;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_vk->vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier barrier =
            makeBarrier(m_vk->image, VkImageLayout(oldLayout), VkImageLayout(newLayout), srcAccess, dstAccess);
        m_vk->vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_vk->vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        const VkResult sub = m_vk->vkQueueSubmit(m_vk->queue, 1, &si, VK_NULL_HANDLE);
        if (sub == VK_SUCCESS)
            m_vk->vkQueueWaitIdle(m_vk->queue);
        m_vk->vkFreeCommandBuffers(m_vk->device, m_vk->cmdPool, 1, &cmd);

        if (sub != VK_SUCCESS)
            return false;
        m_layout = newLayout;
        return true;
    }

    bool GlVkSharedImage::createVulkanImage(QRhi* rhi, int w, int h, bool float16)
    {
        destroyVulkan();
        m_vk = std::make_unique<GlVkSharedImageNative>();

        const auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
        if (!nh || !nh->dev || !nh->physDev || !nh->inst)
        {
            cerr << "ERROR: GlVkSharedImage: no QRhi Vulkan native handles" << endl;
            return false;
        }
        m_vk->device = nh->dev;
        m_vk->physDev = nh->physDev;
        m_vk->queue = nh->gfxQueue;
        m_vk->queueFamily = nh->gfxQueueFamilyIdx;

        m_vk->vkGetDeviceProcAddr = instProc<PFN_vkGetDeviceProcAddr>(nh->inst, "vkGetDeviceProcAddr");
        if (!m_vk->vkGetDeviceProcAddr)
        {
            cerr << "ERROR: GlVkSharedImage: vkGetDeviceProcAddr missing" << endl;
            return false;
        }

        m_vk->vkGetPhysicalDeviceMemoryProperties =
            instProc<PFN_vkGetPhysicalDeviceMemoryProperties>(nh->inst, "vkGetPhysicalDeviceMemoryProperties");
        m_vk->vkCreateImage = devProc<PFN_vkCreateImage>(m_vk.get(), "vkCreateImage");
        m_vk->vkDestroyImage = devProc<PFN_vkDestroyImage>(m_vk.get(), "vkDestroyImage");
        m_vk->vkGetImageMemoryRequirements =
            devProc<PFN_vkGetImageMemoryRequirements>(m_vk.get(), "vkGetImageMemoryRequirements");
        m_vk->vkAllocateMemory = devProc<PFN_vkAllocateMemory>(m_vk.get(), "vkAllocateMemory");
        m_vk->vkFreeMemory = devProc<PFN_vkFreeMemory>(m_vk.get(), "vkFreeMemory");
        m_vk->vkBindImageMemory = devProc<PFN_vkBindImageMemory>(m_vk.get(), "vkBindImageMemory");
        m_vk->vkGetMemoryFdKHR = devProc<PFN_vkGetMemoryFdKHR>(m_vk.get(), "vkGetMemoryFdKHR");
        m_vk->vkCreateCommandPool = devProc<PFN_vkCreateCommandPool>(m_vk.get(), "vkCreateCommandPool");
        m_vk->vkDestroyCommandPool = devProc<PFN_vkDestroyCommandPool>(m_vk.get(), "vkDestroyCommandPool");
        m_vk->vkAllocateCommandBuffers = devProc<PFN_vkAllocateCommandBuffers>(m_vk.get(), "vkAllocateCommandBuffers");
        m_vk->vkFreeCommandBuffers = devProc<PFN_vkFreeCommandBuffers>(m_vk.get(), "vkFreeCommandBuffers");
        m_vk->vkBeginCommandBuffer = devProc<PFN_vkBeginCommandBuffer>(m_vk.get(), "vkBeginCommandBuffer");
        m_vk->vkEndCommandBuffer = devProc<PFN_vkEndCommandBuffer>(m_vk.get(), "vkEndCommandBuffer");
        m_vk->vkCmdPipelineBarrier = devProc<PFN_vkCmdPipelineBarrier>(m_vk.get(), "vkCmdPipelineBarrier");
        m_vk->vkCmdCopyImage = devProc<PFN_vkCmdCopyImage>(m_vk.get(), "vkCmdCopyImage");
        m_vk->vkQueueSubmit = devProc<PFN_vkQueueSubmit>(m_vk.get(), "vkQueueSubmit");
        m_vk->vkQueueWaitIdle = devProc<PFN_vkQueueWaitIdle>(m_vk.get(), "vkQueueWaitIdle");
        m_vk->vkDeviceWaitIdle = devProc<PFN_vkDeviceWaitIdle>(m_vk.get(), "vkDeviceWaitIdle");

        if (!m_vk->vkGetPhysicalDeviceMemoryProperties || !m_vk->vkCreateImage || !m_vk->vkDestroyImage
            || !m_vk->vkGetImageMemoryRequirements || !m_vk->vkAllocateMemory || !m_vk->vkFreeMemory
            || !m_vk->vkBindImageMemory || !m_vk->queue || !m_vk->vkCmdCopyImage)
        {
            cerr << "ERROR: GlVkSharedImage: core Vulkan entry points / queue missing" << endl;
            return false;
        }
        if (!m_vk->vkGetMemoryFdKHR)
        {
            cerr << "ERROR: GlVkSharedImage: vkGetMemoryFdKHR missing (enable VK_KHR_external_memory_fd)" << endl;
            return false;
        }

        const VkFormat format = float16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        // TRANSFER_SRC for GPU copy into the QRhi-owned present texture.
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        VkExternalMemoryImageCreateInfo extImg{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &extImg;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {uint32_t(w), uint32_t(h), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (m_vk->vkCreateImage(m_vk->device, &ici, nullptr, &m_vk->image) != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: vkCreateImage failed" << endl;
            return false;
        }

        VkMemoryRequirements req{};
        m_vk->vkGetImageMemoryRequirements(m_vk->device, m_vk->image, &req);
        m_vk->memSize = req.size;

        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exportInfo;
        dedicated.image = m_vk->image;

        const uint32_t memType = findMemoryType(m_vk.get(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
        {
            cerr << "ERROR: GlVkSharedImage: no DEVICE_LOCAL memory type" << endl;
            return false;
        }

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &dedicated;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType;

        if (m_vk->vkAllocateMemory(m_vk->device, &mai, nullptr, &m_vk->memory) != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: vkAllocateMemory failed" << endl;
            return false;
        }
        if (m_vk->vkBindImageMemory(m_vk->device, m_vk->image, m_vk->memory, 0) != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: vkBindImageMemory failed" << endl;
            return false;
        }

        m_layout = int(VK_IMAGE_LAYOUT_UNDEFINED);
        transitionImage(int(VK_IMAGE_LAYOUT_UNDEFINED), int(VK_IMAGE_LAYOUT_GENERAL), 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        m_layout = int(VK_IMAGE_LAYOUT_GENERAL);
        return true;
    }

    bool GlVkSharedImage::importToGL(QOpenGLContext* glctx, int w, int h, bool float16, int fd, uint64_t memSize)
    {
        destroyGL();

        auto createMem = glProc<PFN_glCreateMemoryObjectsEXT>(glctx, "glCreateMemoryObjectsEXT");
        auto importFd = glProc<PFN_glImportMemoryFdEXT>(glctx, "glImportMemoryFdEXT");
        auto texStorageMem = glProc<PFN_glTextureStorageMem2DEXT>(glctx, "glTextureStorageMem2DEXT");
        auto createTex = glProc<PFN_glCreateTextures>(glctx, "glCreateTextures");
        auto createFbo = glProc<PFN_glCreateFramebuffers>(glctx, "glCreateFramebuffers");
        auto fboTex = glProc<PFN_glNamedFramebufferTexture>(glctx, "glNamedFramebufferTexture");

        if (!createMem || !importFd || !texStorageMem || !createTex)
        {
            cerr << "ERROR: GlVkSharedImage: GL_EXT_memory_object*_fd entry points missing" << endl;
            ::close(fd);
            return false;
        }

        createMem(1, &m_glMem);

        // Vulkan allocates with VkMemoryDedicatedAllocateInfo — GL must mark the
        // memory object dedicated *before* import (EXT_memory_object issue #19).
        // Without this, import/blit often "succeeds" but the image stays black.
        auto memParam = glProc<PFN_glMemoryObjectParameterivEXT>(glctx, "glMemoryObjectParameterivEXT");
        if (memParam)
        {
            const GLint dedicated = GL_TRUE;
            memParam(m_glMem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        }

        // fd is consumed on success
        importFd(m_glMem, GLuint64(memSize), GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);

        createTex(GL_TEXTURE_2D, 1, &m_glTex);
        // Must match Vulkan OPTIMAL tiling *before* TextureStorageMem.
        typedef void(APIENTRY* PFN_glTextureParameteri)(GLuint, GLenum, GLint);
        if (auto texParam = glProc<PFN_glTextureParameteri>(glctx, "glTextureParameteri"))
            texParam(m_glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);

        const GLenum internal = float16 ? GL_RGBA16F : GL_RGBA8;
        texStorageMem(m_glTex, 1, internal, w, h, m_glMem, 0);

        if (createFbo && fboTex)
        {
            createFbo(1, &m_glFbo);
            fboTex(m_glFbo, GL_COLOR_ATTACHMENT0, m_glTex, 0);
            // Completeness is required for blit; incomplete external-memory FBOs
            // "succeed" at blit but leave the image black.
            typedef GLenum(APIENTRY* PFN_glCheckNamedFramebufferStatus)(GLuint, GLenum);
            auto check = glProc<PFN_glCheckNamedFramebufferStatus>(glctx, "glCheckNamedFramebufferStatus");
            if (check)
            {
                const GLenum st = check(m_glFbo, GL_FRAMEBUFFER);
                if (st != GL_FRAMEBUFFER_COMPLETE)
                {
                    cerr << "ERROR: GlVkSharedImage: shared FBO incomplete 0x" << hex << st << dec << endl;
                    destroyGL();
                    return false;
                }
            }
        }
        else
        {
            cerr << "ERROR: GlVkSharedImage: cannot create DSA FBO for blit dest" << endl;
            destroyGL();
            return false;
        }

        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            cerr << "ERROR: GlVkSharedImage: GL import error 0x" << hex << err << dec << endl;
            destroyGL();
            return false;
        }
        return true;
    }

    bool GlVkSharedImage::createSampleImage(QRhi* rhi, int w, int h, bool float16)
    {
        if (!m_vk || !m_vk->device)
            return false;

        const VkFormat format = float16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {uint32_t(w), uint32_t(h), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (m_vk->vkCreateImage(m_vk->device, &ici, nullptr, &m_vk->sampleImage) != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: sample vkCreateImage failed" << endl;
            return false;
        }

        VkMemoryRequirements req{};
        m_vk->vkGetImageMemoryRequirements(m_vk->device, m_vk->sampleImage, &req);
        const uint32_t memType = findMemoryType(m_vk.get(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
            return false;

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType;
        if (m_vk->vkAllocateMemory(m_vk->device, &mai, nullptr, &m_vk->sampleMemory) != VK_SUCCESS)
            return false;
        if (m_vk->vkBindImageMemory(m_vk->device, m_vk->sampleImage, m_vk->sampleMemory, 0) != VK_SUCCESS)
            return false;

        // Wrap for QRhi sampling. Start UNDEFINED; copyToSampleTexture will fill + set layout.
        const QRhiTexture::Format qfmt = float16 ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
        m_sampleTex.reset(rhi->newTexture(qfmt, QSize(w, h), 1, {}));
        QRhiTexture::NativeTexture native;
        native.object = quint64(m_vk->sampleImage);
        native.layout = int(VK_IMAGE_LAYOUT_UNDEFINED);
        if (!m_sampleTex->createFrom(native))
        {
            cerr << "ERROR: GlVkSharedImage: sample QRhiTexture::createFrom failed" << endl;
            m_sampleTex.reset();
            return false;
        }
        return true;
    }

    bool GlVkSharedImage::create(QRhi* rhi, QOpenGLContext* glctx, const QSize& pixelSize, bool float16)
    {
        if (!isSupported(rhi, glctx) || !pixelSize.isValid())
            return false;
        if (valid() && m_size == pixelSize && m_float16 == float16)
            return true;

        destroy();
        m_size = pixelSize;
        m_float16 = float16;
        const int w = pixelSize.width();
        const int h = pixelSize.height();

        if (!createVulkanImage(rhi, w, h, float16))
        {
            destroy();
            return false;
        }
        if (!createSampleImage(rhi, w, h, float16))
        {
            destroy();
            return false;
        }

        int fd = -1;
        VkMemoryGetFdInfoKHR getFd{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        getFd.memory = m_vk->memory;
        getFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (m_vk->vkGetMemoryFdKHR(m_vk->device, &getFd, &fd) != VK_SUCCESS || fd < 0)
        {
            cerr << "ERROR: GlVkSharedImage: vkGetMemoryFdKHR failed" << endl;
            destroy();
            return false;
        }

        if (!importToGL(glctx, w, h, float16, fd, uint64_t(m_vk->memSize)))
        {
            destroy();
            return false;
        }

        cout << "INFO: GL↔Vulkan shared image " << w << "x" << h << (float16 ? " RGBA16F" : " RGBA8")
             << " (GPU interop, no CPU readback)" << endl;
        return true;
    }

    bool GlVkSharedImage::blitFromFramebuffer(GLuint srcFbo, int width, int height)
    {
        // valid() needs sampleTex; allow blit as soon as GL side is up
        if (!m_vk || !m_vk->image || !m_glFbo || width <= 0 || height <= 0)
            return false;
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        if (!ctx)
            return false;

        auto blit = glProc<PFN_glBlitNamedFramebuffer>(ctx, "glBlitNamedFramebuffer");
        auto memBarrier = glProc<PFN_glMemoryBarrier>(ctx, "glMemoryBarrier");

        if (!blit)
        {
            GLint prevRead = 0, prevDraw = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_glFbo);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        }
        else
        {
            blit(srcFbo, m_glFbo, 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        if (memBarrier)
            memBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            static int s_errLog = 0;
            if (s_errLog++ < 5)
                cerr << "ERROR: GlVkSharedImage: blit GL error 0x" << hex << err << dec << " srcFbo=" << srcFbo
                     << " dstFbo=" << m_glFbo << " " << width << "x" << height << endl;
            return false;
        }

        // Do NOT glReadPixels-probe the shared FBO: ReadPixels/GetTexImage on
        // EXT_memory_object stores are not reliably supported and often return
        // zeros even when the GPU write is fine (we used to false-fail here).

        m_layout = int(VK_IMAGE_LAYOUT_GENERAL);
        return true;
    }

    bool GlVkSharedImage::copyToSampleTexture()
    {
        if (!valid() || !m_vk->queue || !ensureCommandPool())
            return false;

        const VkImage dstImage = m_vk->sampleImage;
        // After first copy: SHADER_READ_ONLY. First frame: UNDEFINED.
        VkImageLayout dstOld = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (m_sampleTex)
        {
            const int lay = m_sampleTex->nativeTexture().layout;
            if (lay == int(VK_IMAGE_LAYOUT_UNDEFINED) || lay == 0)
                dstOld = VK_IMAGE_LAYOUT_UNDEFINED;
            else
                dstOld = VkImageLayout(lay);
        }

        const int w = m_size.width();
        const int h = m_size.height();

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_vk->cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (m_vk->vkAllocateCommandBuffers(m_vk->device, &ai, &cmd) != VK_SUCCESS)
            return false;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_vk->vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier barriers[2];
        barriers[0] = makeBarrier(m_vk->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_TRANSFER_READ_BIT);
        barriers[1] =
            makeBarrier(dstImage, dstOld, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        m_vk->vkCmdPipelineBarrier(cmd,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                                       | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = {uint32_t(w), uint32_t(h), 1};
        m_vk->vkCmdCopyImage(cmd, m_vk->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barriers[0] = makeBarrier(m_vk->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        barriers[1] = makeBarrier(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_SHADER_READ_BIT);
        m_vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                       | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 0, nullptr, 2, barriers);

        m_vk->vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        const VkResult sub = m_vk->vkQueueSubmit(m_vk->queue, 1, &si, VK_NULL_HANDLE);
        if (sub == VK_SUCCESS)
            m_vk->vkQueueWaitIdle(m_vk->queue);
        m_vk->vkFreeCommandBuffers(m_vk->device, m_vk->cmdPool, 1, &cmd);

        if (sub != VK_SUCCESS)
        {
            cerr << "ERROR: GlVkSharedImage: copyToSampleTexture submit failed" << endl;
            return false;
        }

        m_layout = int(VK_IMAGE_LAYOUT_GENERAL);
        if (m_sampleTex)
            m_sampleTex->setNativeLayout(int(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        return true;
    }

} // namespace Rv
