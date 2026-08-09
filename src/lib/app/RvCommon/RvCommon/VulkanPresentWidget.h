//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Vulkan/QRhi present surface for the image plane on Wayland.
// Embedded into the main RV window via QWidget::createWindowContainer so it is
// a subsurface (one tiled window), not a separate Hyprland client.
//******************************************************************************
#ifndef __rv_qt__VulkanPresentWidget__h__
#define __rv_qt__VulkanPresentWidget__h__

#include <QWidget>
#include <QImage>
#include <QWindow>
#include <QSize>
#include <memory>
#include <vector>

class QOpenGLContext;

class QRhi;
class QRhiTexture;
class QRhiSampler;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiSwapChain;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QVulkanInstance;

namespace Rv
{
    // Native QWindow that owns a Vulkan QRhi swapchain and draws uploaded frames.
    class VulkanPresentWindow : public QWindow
    {
        Q_OBJECT
    public:
        explicit VulkanPresentWindow();
        ~VulkanPresentWindow() override;

        // RV_HDR_PRESENT: pq | p3linear | srgblinear | p3extended
        enum class PresentMode
        {
            Pq = 0,         // HDR10 ST.2084 (default)
            P3Linear = 1,   // HDRExtendedDisplayP3Linear; buffer assumed PQ codes
            SrgbLinear = 2, // HDRExtendedSrgbLinear; buffer assumed PQ codes
            P3Extended = 3  // P3 linear surface; buffer is P3 + piecewise sRGB TF (macOS EDR)
        };

        void setFrame(QImage img);
        // Float RGBA (top-left origin), for p3extended / EDR headroom (CPU fallback).
        void setFrameFloat(int width, int height, std::vector<float> rgba);
        void setHdrPresent(bool enabled);
        bool hdrPresent() const { return m_hdr; }
        PresentMode presentMode() const { return m_presentMode; }
        // True when the present path requires float transfer (p3extended).
        static bool presentModeNeedsFloatTransfer();

        // GPU interop (no readback): prepare shared image, blit from GL FBO, present.
        // srcFbo is a GL framebuffer name (unsigned = GLuint; avoid OpenGL headers here).
        bool usingGpuInterop() const;
        bool ensureGpuInterop(QOpenGLContext* glctx, const QSize& pixelSize, bool float16);
        bool blitFromGlFramebuffer(unsigned int srcFbo, int width, int height);
        void presentGpuInteropFrame();

    protected:
        void exposeEvent(QExposeEvent*) override;
        void resizeEvent(QResizeEvent*) override;
        bool event(QEvent* e) override;

    private:
        void initRhi();
        void releaseRhi();
        void ensureTexture(const QSize& pixelSize, bool asFloat);
        void ensurePipeline();
        void renderFrame();
        bool ensureSwapChain();
        void selectSwapChainFormat();
        static PresentMode presentModeFromEnv();
        // Compositor SDR white in nits (swapchain / env / 203).
        float resolveSdrWhiteNits() const;
        // Fragment mode: 0 pass, 1 PQ×scale, 2 PQ→linear, 3 sRGB-TF→linear P3
        int presentShaderMode() const;
        float pqSdrWhiteScale() const;
        // After linearization, boost so content 1.0 matches compositor SDR white.
        float linearSdrWhiteMatchScale() const;

        QImage m_pending;
        bool m_hasPending = false;
        std::vector<float> m_pendingFloat;
        int m_pendingFloatW = 0;
        int m_pendingFloatH = 0;
        bool m_hasPendingFloat = false;
        bool m_texIsFloat = false;
        bool m_hdr = false;
        bool m_running = false;
        int m_swapchainFormat = 0; // QRhiSwapChain::Format as int
        PresentMode m_presentMode = PresentMode::Pq;
        // From QRhiSwapChainHdrInfo when available; else env / 203 default.
        float m_sdrWhiteLevelNits = 0.f;

        QVulkanInstance* m_inst = nullptr;
        QRhi* m_rhi = nullptr;
        std::unique_ptr<QRhiSwapChain> m_sc;
        std::unique_ptr<QRhiRenderBuffer> m_ds;
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
        bool m_gpuInterop = false;
        bool m_sharedDirty = false; // GL wrote shared image this frame
        std::unique_ptr<class GlVkSharedImage> m_shared;
    };

    // QWidget wrapper: embeds VulkanPresentWindow via createWindowContainer.
    class VulkanPresentWidget : public QWidget
    {
        Q_OBJECT
    public:
        explicit VulkanPresentWidget(QWidget* parent = nullptr);
        ~VulkanPresentWidget() override;

        void setFrame(QImage img);
        void setFrameFloat(int width, int height, std::vector<float> rgba);
        void setHdrPresent(bool enabled);
        bool hdrPresent() const;
        VulkanPresentWindow* presentWindow() const { return m_window; }
        static bool presentModeNeedsFloatTransfer()
        {
            return VulkanPresentWindow::presentModeNeedsFloatTransfer();
        }
        bool usingGpuInterop() const;
        bool ensureGpuInterop(QOpenGLContext* glctx, const QSize& pixelSize, bool float16);
        bool blitFromGlFramebuffer(unsigned int srcFbo, int width, int height);
        void presentGpuInteropFrame();

    private:
        VulkanPresentWindow* m_window = nullptr;
        QWidget* m_container = nullptr;
    };

} // namespace Rv

#endif
