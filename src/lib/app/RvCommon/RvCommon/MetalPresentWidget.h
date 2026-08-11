//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Metal/QRhi EDR present surface for the image plane on macOS.
//
// The macOS counterpart of VulkanPresentWidget. Embedded into the main RV
// window via QWidget::createWindowContainer so it is a child surface of the
// same window, not a second top-level. Validated by _hdr_test/spike_b.
//
// Surface model: QRhiSwapChain::HDRExtendedDisplayP3Linear, which Qt maps to
// MTLPixelFormatRGBA16Float + kCGColorSpaceExtendedLinearDisplayP3 +
// wantsExtendedDynamicRangeContent. In that space linear 1.0 *is* SDR white and
// values above it are the extended range, so the Wayland SDR-white match that
// VulkanPresentWindow applies must NOT be applied here.
//
// Frames arrive as RGBA16F from GLView's float present FBO and are decoded with
// the shared present shader in mode 3 (piecewise sRGB EOTF -> linear Display
// P3), which the Linux work already wrote and validated for exactly this
// surface model.
//******************************************************************************
#ifndef __rv_qt__MetalPresentWidget__h__
#define __rv_qt__MetalPresentWidget__h__

#include <RvCommon/PresentSurface.h>

#include <QWidget>
#include <QImage>
#include <QWindow>
#include <QSize>
#include <cstdint>
#include <memory>
#include <vector>

class QRhi;
class QRhiTexture;
class QRhiSampler;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiSwapChain;
class QRhiRenderPassDescriptor;

namespace Rv
{

    // Native QWindow owning a Metal QRhi swapchain.
    class MetalPresentWindow : public QWindow
    {
        Q_OBJECT
    public:
        explicit MetalPresentWindow();
        ~MetalPresentWindow() override;

        void setFrame(QImage img);
        void setFrameHalf(int width, int height, std::vector<uint16_t> rgba16);
        void setHdrPresent(bool enabled);
        bool hdrPresent() const { return m_hdr; }

        // True once an EDR-capable swapchain is actually in use, which is what
        // makes the float transfer worth doing.
        bool edrSurfaceActive() const { return m_edrSurface; }

        // Current EDR headroom of the display this window is on. 1.0 means no
        // headroom. Read from NSScreen, never QRhiSwapChain::hdrInfo(): Qt's
        // Metal backend reports a hardcoded dummy sdrWhiteLevel of 200.
        float edrHeadroom() const { return m_edrHeadroom; }

    protected:
        void exposeEvent(QExposeEvent*) override;
        void resizeEvent(QResizeEvent*) override;
        bool event(QEvent* e) override;

    private:
        void initRhi();
        void releaseRhi();
        void ensureTexture(const QSize& pixelSize, bool asFloat);
        void ensurePipeline();
        bool ensureSwapChain();
        void renderFrame();
        void refreshEdrInfo();

        QImage m_pending;
        bool m_hasPending = false;
        std::vector<uint16_t> m_pendingHalf;
        int m_pendingHalfW = 0;
        int m_pendingHalfH = 0;
        bool m_hasPendingHalf = false;
        bool m_texIsFloat = false;
        // Orientation of whatever is currently in m_tex. Must persist with the
        // texture: repaints that carry no new frame still re-upload the
        // uniform, and a per-frame local would flip the old texture back.
        float m_flipV = 0.f;
        bool m_hdr = false;
        bool m_running = false;
        bool m_edrSurface = false;
        float m_edrHeadroom = 1.f;

        QRhi* m_rhi = nullptr;
        std::unique_ptr<QRhiSwapChain> m_sc;
        std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
        std::unique_ptr<QRhiTexture> m_tex;
        std::unique_ptr<QRhiSampler> m_sampler;
        std::unique_ptr<QRhiBuffer> m_vbuf;
        std::unique_ptr<QRhiBuffer> m_ubuf;
        std::unique_ptr<QRhiShaderResourceBindings> m_srb;
        std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
        QSize m_texSize;
        bool m_pipelineBuilt = false;
        bool m_vbufUploaded = false;
    };

    // QWidget wrapper: embeds MetalPresentWindow via createWindowContainer.
    class MetalPresentWidget
        : public QWidget
        , public PresentSurface
    {
        Q_OBJECT
    public:
        explicit MetalPresentWidget(QWidget* parent = nullptr);
        ~MetalPresentWidget() override;

        void setFrame(QImage img) override;
        void setFrameHalf(int width, int height, std::vector<uint16_t> rgba16) override;
        void setHdrPresent(bool enabled) override;
        bool hdrPresent() const override;

        // EDR wants the RGBA16F path: an 8-bit grab cannot carry values > 1.0.
        bool needsFloatTransfer() const override;

        // No GPU interop yet. The IOSurface path described in
        // _hdr_test/HDR-MACOS-EDR.md is the intended replacement for the
        // Linux fd-based GL<->Vk sharing; until then GLView falls back to the
        // CPU readback path, which these return false/no-op to select.
        bool usingGpuInterop() const override { return false; }
        bool ensureGpuInterop(QOpenGLContext*, const QSize&, bool) override { return false; }
        bool blitFromGlFramebuffer(unsigned int, int, int) override { return false; }
        void presentGpuInteropFrame() override {}

        MetalPresentWindow* presentWindow() const { return m_window; }

    private:
        MetalPresentWindow* m_window = nullptr;
        QWidget* m_container = nullptr;
    };

} // namespace Rv

#endif
