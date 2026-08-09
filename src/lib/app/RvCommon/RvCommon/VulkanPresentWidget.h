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
#include <memory>

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

        void setFrame(QImage img);
        void setHdrPresent(bool enabled);
        bool hdrPresent() const { return m_hdr; }

    protected:
        void exposeEvent(QExposeEvent*) override;
        void resizeEvent(QResizeEvent*) override;
        bool event(QEvent* e) override;

    private:
        void initRhi();
        void releaseRhi();
        void ensureTexture(const QSize& pixelSize);
        void ensurePipeline();
        void renderFrame();
        bool ensureSwapChain();
        void selectSwapChainFormat();

        QImage m_pending;
        bool m_hasPending = false;
        bool m_hdr = false;
        bool m_running = false;
        int m_swapchainFormat = 0; // QRhiSwapChain::Format as int

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
    };

    // QWidget wrapper: embeds VulkanPresentWindow via createWindowContainer.
    class VulkanPresentWidget : public QWidget
    {
        Q_OBJECT
    public:
        explicit VulkanPresentWidget(QWidget* parent = nullptr);
        ~VulkanPresentWidget() override;

        void setFrame(QImage img);
        void setHdrPresent(bool enabled);
        bool hdrPresent() const;
        VulkanPresentWindow* presentWindow() const { return m_window; }

    private:
        VulkanPresentWindow* m_window = nullptr;
        QWidget* m_container = nullptr;
    };

} // namespace Rv

#endif
