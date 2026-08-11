//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Backend-neutral interface for an external present surface stacked over
// GLView.
//
// GLView renders into a GL FBO and hands the result to a platform present
// surface that owns its own graphics API and its own colour-managed swapchain.
// The surface is a sibling QWindow embedded via createWindowContainer, not a
// separate top-level window.
//
// Backends:
//   Linux   VulkanPresentWidget  — Vulkan/QRhi, Wayland colour management,
//                                  GL<->Vk sharing via POSIX fd external memory
//   macOS   MetalPresentWidget   — Metal/QRhi, EDR via extended-linear
//                                  colorspaces (planned: IOSurface sharing)
//
// Deliberately NOT a QObject: implementations derive from QWidget (which is a
// QObject) *and* this interface. Recover it with dynamic_cast from the QWidget,
// not qobject_cast. Where a QObject is needed (e.g. a QTimer context), use the
// widget pointer itself.
//
// See _hdr_test/HDR-SURFACE-DESIGN.md.
//******************************************************************************
#ifndef __rv_qt__PresentSurface__h__
#define __rv_qt__PresentSurface__h__

#include <QImage>
#include <QSize>
#include <cstdint>
#include <vector>

class QOpenGLContext;

namespace Rv
{

    class PresentSurface
    {
    public:
        virtual ~PresentSurface() = default;

        // Frame upload (CPU path). setFrameHalf takes IEEE binary16 RGBA with a
        // top-left origin, matching the GL RGBA16F present FBO.
        virtual void setFrame(QImage img) = 0;
        virtual void setFrameHalf(int width, int height, std::vector<uint16_t> rgba16) = 0;

        // Request HDR presentation. Whether it is honoured depends on the
        // backend and the display; query hdrPresent() afterwards.
        virtual void setHdrPresent(bool enabled) = 0;
        virtual bool hdrPresent() const = 0;

        // True when this backend's present mode needs float (RGBA16F) transfer
        // rather than an 8-bit grab. Drives GLView's float FBO allocation.
        virtual bool needsFloatTransfer() const = 0;

        // GPU interop (no readback): share an image with the present device,
        // blit into it from a GL FBO, then present. Backends without interop
        // return false from ensureGpuInterop and GLView falls back to CPU
        // transfer. srcFbo is a GLuint, passed untyped to keep OpenGL headers
        // out of this interface.
        virtual bool usingGpuInterop() const = 0;
        virtual bool ensureGpuInterop(QOpenGLContext* glctx, const QSize& pixelSize,
                                      bool float16) = 0;
        virtual bool blitFromGlFramebuffer(unsigned int srcFbo, int width, int height) = 0;
        virtual void presentGpuInteropFrame() = 0;
    };

} // namespace Rv

#endif
