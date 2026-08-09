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
#include <vector>

using namespace std;

// GL_EXT_memory_object / _fd (not always on QOpenGLExtraFunctions)
typedef void(APIENTRY* PFN_glCreateMemoryObjectsEXT)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glDeleteMemoryObjectsEXT)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFN_glImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint);
typedef void(APIENTRY* PFN_glTextureStorageMem2DEXT)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void(APIENTRY* PFN_glCreateTextures)(GLenum, GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glCreateFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_glNamedFramebufferTexture)(GLuint, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFN_glBlitNamedFramebuffer)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                                   GLint, GLbitfield, GLenum);

#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif

// Qt builds with VK_NO_PROTOTYPES — load every Vulkan entry point we need.
struct GlVkSharedImageNative
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memSize = 0;

    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = nullptr;
};

namespace Rv
{
    namespace
    {
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

    bool GlVkSharedImage::valid() const { return m_vk && m_vk->image && m_glTex && m_rhiTex; }

    void GlVkSharedImage::destroy()
    {
        // Drop QRhi wrap before freeing the native VkImage it references.
        m_rhiTex.reset();
        destroyGL();
        destroyVulkan();
        m_size = QSize();
        m_float16 = false;
    }

    void GlVkSharedImage::destroyGL()
    {
        // GL context may already be gone; best-effort delete if current.
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
            if (m_vk->image && m_vk->vkDestroyImage)
                m_vk->vkDestroyImage(m_vk->device, m_vk->image, nullptr);
            if (m_vk->memory && m_vk->vkFreeMemory)
                m_vk->vkFreeMemory(m_vk->device, m_vk->memory, nullptr);
        }
        m_vk.reset();
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

        m_vk->vkGetDeviceProcAddr = instProc<PFN_vkGetDeviceProcAddr>(nh->inst, "vkGetDeviceProcAddr");
        if (!m_vk->vkGetDeviceProcAddr)
        {
            cerr << "ERROR: GlVkSharedImage: vkGetDeviceProcAddr missing" << endl;
            return false;
        }

        // Core device/instance entry points (loaded via instance or device proc addr).
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

        if (!m_vk->vkGetPhysicalDeviceMemoryProperties || !m_vk->vkCreateImage || !m_vk->vkDestroyImage
            || !m_vk->vkGetImageMemoryRequirements || !m_vk->vkAllocateMemory || !m_vk->vkFreeMemory
            || !m_vk->vkBindImageMemory)
        {
            cerr << "ERROR: GlVkSharedImage: core Vulkan entry points missing" << endl;
            return false;
        }
        if (!m_vk->vkGetMemoryFdKHR)
        {
            cerr << "ERROR: GlVkSharedImage: vkGetMemoryFdKHR missing (enable VK_KHR_external_memory_fd)" << endl;
            return false;
        }

        const VkFormat format = float16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

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
        // fd is consumed on success
        importFd(m_glMem, GLuint64(memSize), GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);

        createTex(GL_TEXTURE_2D, 1, &m_glTex);
        const GLenum internal = float16 ? GL_RGBA16F : GL_RGBA8;
        texStorageMem(m_glTex, 1, internal, w, h, m_glMem, 0);

        if (createFbo && fboTex)
        {
            createFbo(1, &m_glFbo);
            fboTex(m_glFbo, GL_COLOR_ATTACHMENT0, m_glTex, 0);
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

        // Wrap VkImage for QRhi sampling. GENERAL keeps interop simple with glFinish.
        const QRhiTexture::Format qfmt = float16 ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
        m_rhiTex.reset(rhi->newTexture(qfmt, pixelSize, 1, {}));
        QRhiTexture::NativeTexture native;
        native.object = quint64(m_vk->image);
        native.layout = int(VK_IMAGE_LAYOUT_GENERAL);
        if (!m_rhiTex->createFrom(native))
        {
            cerr << "ERROR: GlVkSharedImage: QRhiTexture::createFrom failed" << endl;
            destroy();
            return false;
        }

        cout << "INFO: GL↔Vulkan shared image " << w << "x" << h << (float16 ? " RGBA16F" : " RGBA8")
             << " (GPU interop, no CPU readback)" << endl;
        return true;
    }

    bool GlVkSharedImage::blitFromFramebuffer(GLuint srcFbo, int width, int height)
    {
        if (!valid() || !m_glFbo || width <= 0 || height <= 0)
            return false;
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        if (!ctx)
            return false;

        auto blit = glProc<PFN_glBlitNamedFramebuffer>(ctx, "glBlitNamedFramebuffer");
        if (!blit)
        {
            // Fallback: bind FBOs and glBlitFramebuffer
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
        return glGetError() == GL_NO_ERROR;
    }

} // namespace Rv
