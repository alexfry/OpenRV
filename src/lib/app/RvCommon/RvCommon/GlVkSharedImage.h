//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// GPU-only GL ↔ Vulkan color image sharing via opaque FD external memory.
// Vulkan allocates exportable image; GL imports it. No CPU readback.
//******************************************************************************
#ifndef __rv_qt__GlVkSharedImage__h__
#define __rv_qt__GlVkSharedImage__h__

#include <QByteArrayList>
#include <QSize>
#include <QtGui/qopengl.h>
#include <cstdint>
#include <memory>

class QRhi;
class QRhiTexture;
class QOpenGLContext;

// Avoid pulling vulkan.h into every translation unit that includes this.
struct GlVkSharedImageNative;

namespace Rv
{
    class GlVkSharedImage
    {
    public:
        GlVkSharedImage();
        ~GlVkSharedImage();

        GlVkSharedImage(const GlVkSharedImage&) = delete;
        GlVkSharedImage& operator=(const GlVkSharedImage&) = delete;

        // Request device extensions that must be enabled on the QRhi Vulkan device.
        static QByteArrayList requiredDeviceExtensions();

        // true if GL + Vulkan external memory FD path is usable in this process.
        static bool isSupported(QRhi* rhi, QOpenGLContext* glctx);

        // Create/recreate shared image. float16 => RGBA16F, else RGBA8.
        // Requires GL context current and a Vulkan QRhi with external-memory extensions.
        bool create(QRhi* rhi, QOpenGLContext* glctx, const QSize& pixelSize, bool float16);

        void destroy();

        bool valid() const;
        QSize size() const { return m_size; }
        bool isFloat16() const { return m_float16; }

        GLuint glTexture() const { return m_glTex; }
        // QRhi texture wrapping the same VkImage (for sampling in present pass).
        QRhiTexture* rhiTexture() const { return m_rhiTex.get(); }

        // GPU blit from an existing GL FBO's color0 into the shared texture.
        // GL context must be current. Does not glFinish.
        bool blitFromFramebuffer(GLuint srcFbo, int width, int height);

    private:
        bool createVulkanImage(QRhi* rhi, int w, int h, bool float16);
        bool importToGL(QOpenGLContext* glctx, int w, int h, bool float16, int fd, uint64_t memSize);
        void destroyGL();
        void destroyVulkan();

        QSize m_size;
        bool m_float16 = false;
        GLuint m_glTex = 0;
        GLuint m_glMem = 0;
        GLuint m_glFbo = 0; // FBO wrapping m_glTex for blit destination
        std::unique_ptr<QRhiTexture> m_rhiTex;
        std::unique_ptr<GlVkSharedImageNative> m_vk;
    };
} // namespace Rv

#endif
