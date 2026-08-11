//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// GL <-> Metal zero-copy image sharing via IOSurface. The macOS counterpart of
// GlVkSharedImage.
//
// GlVkSharedImage shares an allocation through POSIX file-descriptor external
// memory (vkGetMemoryFdKHR + glImportMemoryFdEXT), which does not exist on
// macOS. IOSurface is the platform equivalent and is simpler: one surface can
// be bound as a GL texture with CGLTexImageIOSurface2D and as an MTLTexture
// with -newTextureWithDescriptor:iosurface:plane:, both referring to the same
// memory.
//
// Without this, GLView reads the float FBO back with glReadPixels and uploads
// it again every frame -- ~18.6 MB per frame at 2016x1152 RGBA16F, plus a
// synchronous stall.
//
// Note the GL side binds to GL_TEXTURE_RECTANGLE, not GL_TEXTURE_2D:
// CGLTexImageIOSurface2D only accepts the rectangle target. That is invisible
// to the present shader because the GL side only ever blits into this texture
// through an FBO; sampling happens on the Metal side.
//******************************************************************************
#ifndef __rv_qt__IOSurfaceSharedImage__h__
#define __rv_qt__IOSurfaceSharedImage__h__

#include <QSize>
#include <QtGui/qopengl.h>
#include <cstdint>
#include <memory>

class QRhi;
class QRhiTexture;
class QOpenGLContext;

// Avoid pulling IOSurface/Metal headers into every translation unit.
struct IOSurfaceSharedImageNative;

namespace Rv
{

    class IOSurfaceSharedImage
    {
    public:
        IOSurfaceSharedImage();
        ~IOSurfaceSharedImage();

        IOSurfaceSharedImage(const IOSurfaceSharedImage&) = delete;
        IOSurfaceSharedImage& operator=(const IOSurfaceSharedImage&) = delete;

        // True if this process can do the GL<->Metal share at all: needs a Metal
        // QRhi and a current GL context whose CGL context we can reach.
        static bool isSupported(QRhi* rhi, QOpenGLContext* glctx);

        // Create/recreate the shared surface. float16 => RGBA16F (required for
        // EDR values above 1.0), else RGBA8.
        bool create(QRhi* rhi, QOpenGLContext* glctx, const QSize& pixelSize, bool float16);

        void destroy();

        bool valid() const;
        QSize size() const { return m_size; }
        bool isFloat16() const { return m_float16; }

        // QRhi texture wrapping the Metal view of the surface, for the present
        // pipeline to sample.
        QRhiTexture* sampleTexture() const { return m_sampleTex.get(); }

        // GPU blit from a GL FBO's colour attachment 0 into the shared surface.
        // The GL context must be current. Flushes so Metal sees the writes.
        bool blitFromFramebuffer(GLuint srcFbo, int width, int height);

    private:
        void destroyGL();

        QSize m_size;
        bool m_float16 = false;
        GLuint m_glTex = 0; // GL_TEXTURE_RECTANGLE
        GLuint m_glFbo = 0; // FBO wrapping m_glTex, blit destination
        std::unique_ptr<QRhiTexture> m_sampleTex;
        std::unique_ptr<IOSurfaceSharedImageNative> m_native;
    };

} // namespace Rv

#endif
