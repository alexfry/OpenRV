//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <RvCommon/IOSurfaceSharedImage.h>

#include <rhi/qrhi.h>

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <OpenGL/CGLIOSurface.h>

#include <iostream>

// gl3.h does not declare the rectangle target in all SDK versions.
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

struct IOSurfaceSharedImageNative
{
    IOSurfaceRef surface = nullptr;
    id<MTLTexture> mtlTex = nil;

    ~IOSurfaceSharedImageNative()
    {
        mtlTex = nil; // ARC or manual: released with the owning autorelease pool
        if (surface)
        {
            CFRelease(surface);
            surface = nullptr;
        }
    }
};

namespace Rv
{
    using namespace std;

    namespace
    {
        // kCVPixelFormatType_64RGBAHalf / _32RGBA, spelled directly so this file
        // does not need CoreVideo.
        constexpr uint32_t kPixelFormatRGBAHalf = 'RGhA';
        constexpr uint32_t kPixelFormatRGBA8 = 'RGBA';

        id<MTLDevice> metalDeviceOf(QRhi* rhi)
        {
            if (!rhi || rhi->backend() != QRhi::Metal)
                return nil;
            const auto* nh = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
            if (!nh)
                return nil;
            return (__bridge id<MTLDevice>)nh->dev;
        }
    } // namespace

    IOSurfaceSharedImage::IOSurfaceSharedImage()
        : m_native(new IOSurfaceSharedImageNative)
    {
    }

    IOSurfaceSharedImage::~IOSurfaceSharedImage() { destroy(); }

    bool IOSurfaceSharedImage::isSupported(QRhi* rhi, QOpenGLContext* glctx)
    {
        if (!metalDeviceOf(rhi))
            return false;
        if (!glctx || !QOpenGLContext::currentContext())
            return false;
        // CGLTexImageIOSurface2D needs a CGL context, which exists for Qt's
        // NSOpenGLContext-backed GL on macOS.
        return CGLGetCurrentContext() != nullptr;
    }

    bool IOSurfaceSharedImage::valid() const
    {
        return m_native && m_native->surface && m_native->mtlTex && m_glTex != 0
               && m_sampleTex != nullptr;
    }

    bool IOSurfaceSharedImage::create(QRhi* rhi, QOpenGLContext* glctx, const QSize& pixelSize,
                                      bool float16)
    {
        if (!pixelSize.isValid() || pixelSize.width() <= 0 || pixelSize.height() <= 0)
            return false;
        if (valid() && m_size == pixelSize && m_float16 == float16)
            return true;

        destroy();

        id<MTLDevice> dev = metalDeviceOf(rhi);
        if (!dev)
            return false;
        if (!glctx || !QOpenGLContext::currentContext() || !CGLGetCurrentContext())
            return false;

        const int w = pixelSize.width();
        const int h = pixelSize.height();
        const size_t bpe = float16 ? 8 : 4; // bytes per element (RGBA)

        @autoreleasepool
        {
            // ---- the shared allocation -------------------------------------
            const size_t bytesPerRow =
                IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(w) * bpe);
            const size_t allocSize =
                IOSurfaceAlignProperty(kIOSurfaceAllocSize, bytesPerRow * size_t(h));

            NSDictionary* props = @{
                (id)kIOSurfaceWidth : @(w),
                (id)kIOSurfaceHeight : @(h),
                (id)kIOSurfaceBytesPerElement : @(bpe),
                (id)kIOSurfaceBytesPerRow : @(bytesPerRow),
                (id)kIOSurfaceAllocSize : @(allocSize),
                (id)kIOSurfacePixelFormat :
                    @(float16 ? kPixelFormatRGBAHalf : kPixelFormatRGBA8),
            };

            IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            if (!surf)
            {
                cerr << "ERROR: IOSurfaceSharedImage: IOSurfaceCreate failed" << endl;
                return false;
            }
            m_native->surface = surf; // owned

            // ---- Metal view -------------------------------------------------
            MTLTextureDescriptor* desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:(float16 ? MTLPixelFormatRGBA16Float
                                                            : MTLPixelFormatRGBA8Unorm)
                                             width:w
                                            height:h
                                         mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModeManaged;

            id<MTLTexture> mtl = [dev newTextureWithDescriptor:desc iosurface:surf plane:0];
            if (!mtl)
            {
                cerr << "ERROR: IOSurfaceSharedImage: newTextureWithDescriptor:iosurface: failed"
                     << endl;
                destroy();
                return false;
            }
            m_native->mtlTex = mtl;

            // ---- QRhi wrapper so the present pipeline can sample it ----------
            m_sampleTex.reset(rhi->newTexture(float16 ? QRhiTexture::RGBA16F
                                                      : QRhiTexture::RGBA8,
                                              pixelSize));
            if (!m_sampleTex->createFrom({quint64(reinterpret_cast<uintptr_t>(mtl)), 0}))
            {
                cerr << "ERROR: IOSurfaceSharedImage: QRhiTexture::createFrom failed" << endl;
                destroy();
                return false;
            }

            // ---- GL view ----------------------------------------------------
            auto* f = QOpenGLContext::currentContext()->functions();
            glGenTextures(1, &m_glTex);
            glBindTexture(GL_TEXTURE_RECTANGLE, m_glTex);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const CGLError err = CGLTexImageIOSurface2D(
                CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE,
                float16 ? GL_RGBA16F : GL_RGBA8, w, h, GL_RGBA,
                float16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, surf, 0);
            glBindTexture(GL_TEXTURE_RECTANGLE, 0);

            if (err != kCGLNoError)
            {
                cerr << "ERROR: IOSurfaceSharedImage: CGLTexImageIOSurface2D failed (" << int(err)
                     << ")" << endl;
                destroy();
                return false;
            }

            // FBO wrapping the shared texture, used as the blit destination.
            f->glGenFramebuffers(1, &m_glFbo);
            f->glBindFramebuffer(GL_FRAMEBUFFER, m_glFbo);
            f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE,
                                      m_glTex, 0);
            const GLenum status = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
            f->glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                cerr << "ERROR: IOSurfaceSharedImage: shared FBO incomplete (0x" << std::hex
                     << status << std::dec << ")" << endl;
                destroy();
                return false;
            }
        }

        m_size = pixelSize;
        m_float16 = float16;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            cout << "INFO: IOSurface GL<->Metal share " << w << "x" << h << " "
                 << (float16 ? "RGBA16F" : "RGBA8") << " (zero-copy present, no readback)"
                 << endl;
        }
        return true;
    }

    bool IOSurfaceSharedImage::blitFromFramebuffer(GLuint srcFbo, int width, int height)
    {
        if (!valid() || width <= 0 || height <= 0)
            return false;
        auto* ctx = QOpenGLContext::currentContext();
        if (!ctx)
            return false;
        auto* f = ctx->functions();

        const int w = std::min(width, m_size.width());
        const int h = std::min(height, m_size.height());

        GLint prevRead = 0, prevDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_glFbo);

        // Straight copy, no flip: orientation is handled in the present shader
        // via the flipV uniform, same as the CPU path.
        auto* extra = ctx->extraFunctions();
        extra->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevRead));
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prevDraw));

        // Metal reads this surface on a different queue. Flush so the writes are
        // actually visible; IOSurface itself provides the coherence.
        glFlush();
        return true;
    }

    void IOSurfaceSharedImage::destroyGL()
    {
        if (QOpenGLContext::currentContext())
        {
            auto* f = QOpenGLContext::currentContext()->functions();
            if (m_glFbo)
                f->glDeleteFramebuffers(1, &m_glFbo);
            if (m_glTex)
                glDeleteTextures(1, &m_glTex);
        }
        m_glFbo = 0;
        m_glTex = 0;
    }

    void IOSurfaceSharedImage::destroy()
    {
        m_sampleTex.reset();
        destroyGL();
        if (m_native)
        {
            m_native->mtlTex = nil;
            if (m_native->surface)
            {
                CFRelease(m_native->surface);
                m_native->surface = nullptr;
            }
        }
        m_size = QSize();
    }

} // namespace Rv
