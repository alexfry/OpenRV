//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <RvCommon/VulkanPresentWidget.h>

// Vulkan headers must be visible before qrhi_platform.h enables QRhiVulkanInitParams.
#include <vulkan/vulkan.h>

#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <rhi/qrhi_platform.h>

#include <QColorSpace>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QSurfaceFormat>
#include <QVulkanInstance>
#include <QVBoxLayout>
#include <cstring>
#include <iostream>

#include "shaders/present_vert_qsb.h"
#include "shaders/present_frag_qsb.h"

namespace Rv
{
    using namespace std;

    namespace
    {
        // NDC Y-up quad, UVs for a *top-left origin* image (QImage / grabFramebuffer).
        // TL, TR, BL, BR — triangle strip.
        struct Vtx
        {
            float x, y, u, v;
        };
        static const Vtx kQuad[4] = {
            {-1.f, 1.f, 0.f, 0.f}, // top-left
            {1.f, 1.f, 1.f, 0.f},  // top-right
            {-1.f, -1.f, 0.f, 1.f}, // bottom-left
            {1.f, -1.f, 1.f, 1.f},  // bottom-right
        };
    } // namespace

    // -------------------------------------------------------------------------
    // VulkanPresentWindow
    // -------------------------------------------------------------------------

    VulkanPresentWindow::VulkanPresentWindow()
        : QWindow()
    {
        setSurfaceType(QSurface::VulkanSurface);
    }

    VulkanPresentWindow::~VulkanPresentWindow() { releaseRhi(); }

    void VulkanPresentWindow::setHdrPresent(bool enabled)
    {
        m_hdr = enabled;
        QSurfaceFormat fmt = format();
        if (m_hdr)
            fmt.setColorSpace(QColorSpace(QColorSpace::Bt2100Pq));
        else
            fmt.setColorSpace(QColorSpace(QColorSpace::SRgb));
        setFormat(fmt);
        cout << "INFO: VulkanPresentWindow HDR present " << (enabled ? "ON (Bt2100Pq request)" : "OFF") << endl;
    }

    void VulkanPresentWindow::setFrame(QImage img)
    {
        if (img.isNull())
            return;
        if (img.format() != QImage::Format_RGBA8888)
            img = img.convertToFormat(QImage::Format_RGBA8888);
        // grabFramebuffer() returns device pixels; DPR metadata must not affect
        // upload size — treat buffer as 1:1 pixels for the texture.
        img.setDevicePixelRatio(1.0);
        m_pending = std::move(img);
        m_hasPending = true;
        if (isExposed())
            requestUpdate();
    }

    void VulkanPresentWindow::initRhi()
    {
        if (m_rhi)
            return;

        m_inst = new QVulkanInstance;
        m_inst->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (!m_inst->create())
        {
            cerr << "ERROR: QVulkanInstance::create failed" << endl;
            delete m_inst;
            m_inst = nullptr;
            return;
        }
        setVulkanInstance(m_inst);

        QRhiVulkanInitParams params;
        params.inst = m_inst;
        params.window = this;
        m_rhi = QRhi::create(QRhi::Vulkan, &params);
        if (!m_rhi)
        {
            cerr << "ERROR: QRhi::create(Vulkan) failed" << endl;
            return;
        }
        cout << "INFO: VulkanPresentWindow RHI backend=" << m_rhi->backendName()
             << " platform=" << QGuiApplication::platformName().toStdString()
             << " dpr=" << devicePixelRatio() << " size=" << width() << "x" << height()
             << " yUpNDC=" << m_rhi->isYUpInNDC() << " yUpFB=" << m_rhi->isYUpInFramebuffer() << endl;

        m_sc.reset(m_rhi->newSwapChain());
        m_ds.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(), 1,
                                          QRhiRenderBuffer::UsedWithSwapChainOnly));
        m_sc->setWindow(this);
        m_sc->setDepthStencil(m_ds.get());
        selectSwapChainFormat();
        m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
        m_sc->setRenderPassDescriptor(m_rp.get());

        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();

        // Static quad vertex buffer
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
        m_vbuf->create();

        // Uniform buffer for clipSpaceCorrMatrix
        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
        m_ubuf->create();
    }

    void VulkanPresentWindow::selectSwapChainFormat()
    {
        if (!m_sc)
            return;

        // Default SDR.
        QRhiSwapChain::Format fmt = QRhiSwapChain::SDR;
        const char* name = "SDR";

        if (m_hdr)
        {
            // Prefer HDR10 (PQ / ST.2084) — matches DisplayIPNode SMPTE-2084 encode
            // and mpv's VK_COLOR_SPACE_HDR10_ST2084_EXT path on this machine.
            if (m_sc->isFormatSupported(QRhiSwapChain::HDR10))
            {
                fmt = QRhiSwapChain::HDR10;
                name = "HDR10 (PQ/ST.2084)";
            }
            else if (m_sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear))
            {
                fmt = QRhiSwapChain::HDRExtendedSrgbLinear;
                name = "HDRExtendedSrgbLinear";
                cout << "WARNING: HDR10 unsupported; using scRGB linear. "
                        "PQ-encoded FBOs will look wrong until encode matches."
                     << endl;
            }
            else if (m_sc->isFormatSupported(QRhiSwapChain::HDRExtendedDisplayP3Linear))
            {
                fmt = QRhiSwapChain::HDRExtendedDisplayP3Linear;
                name = "HDRExtendedDisplayP3Linear";
                cout << "WARNING: HDR10 unsupported; using P3 linear." << endl;
            }
            else
            {
                cout << "WARNING: no HDR swapchain format supported — presenting as SDR "
                        "(highlights will not be absolute nits)"
                     << endl;
            }
        }

        m_sc->setFormat(fmt);
        if (int(fmt) != m_swapchainFormat)
        {
            m_swapchainFormat = int(fmt);
            cout << "INFO: VulkanPresentWindow swapchain format=" << name << endl;
        }
    }

    void VulkanPresentWindow::releaseRhi()
    {
        m_pipeline.reset();
        m_srb.reset();
        m_tex.reset();
        m_sampler.reset();
        m_ubuf.reset();
        m_vbuf.reset();
        m_rp.reset();
        m_ds.reset();
        m_sc.reset();
        delete m_rhi;
        m_rhi = nullptr;
        if (m_inst)
        {
            setVulkanInstance(nullptr);
            delete m_inst;
            m_inst = nullptr;
        }
        m_pipelineBuilt = false;
        m_texSize = QSize();
        m_running = false;
        m_vbufUploaded = false;
    }

    bool VulkanPresentWindow::ensureSwapChain()
    {
        if (!m_rhi || !m_sc)
            return false;
        if (size().isEmpty())
            return false;

        // Re-apply HDR format each recreate (createOrResize can reset state).
        selectSwapChainFormat();

        // QRhi derives pixel size from the QWindow (logical size × dpr).
        if (!m_sc->createOrResize())
        {
            cerr << "ERROR: swapchain createOrResize failed" << endl;
            return false;
        }

        static bool loggedHdrInfo = false;
        if (m_hdr && !loggedHdrInfo)
        {
            loggedHdrInfo = true;
            const QRhiSwapChainHdrInfo hi = m_sc->hdrInfo();
            cout << "INFO: swapchain HDR info: limitsType=" << int(hi.limitsType)
                 << " sdrWhiteLevel=" << hi.sdrWhiteLevel;
            if (hi.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits)
            {
                cout << " minNits=" << hi.limits.luminanceInNits.minLuminance
                     << " maxNits=" << hi.limits.luminanceInNits.maxLuminance;
            }
            cout << " formatEnum=" << int(m_sc->format()) << endl;
        }

        m_pipeline.reset();
        m_pipelineBuilt = false;
        return true;
    }

    void VulkanPresentWindow::ensureTexture(const QSize& pixelSize)
    {
        if (!m_rhi || !pixelSize.isValid())
            return;
        if (m_tex && m_texSize == pixelSize)
            return;
        m_tex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1, {}));
        if (!m_tex->create())
        {
            cerr << "ERROR: present texture create failed" << endl;
            m_tex.reset();
            return;
        }
        m_texSize = pixelSize;
        m_srb.reset();
        m_pipeline.reset();
        m_pipelineBuilt = false;
    }

    void VulkanPresentWindow::ensurePipeline()
    {
        if (!m_rhi || m_pipelineBuilt || !m_tex || !m_sampler || !m_rp || !m_ubuf)
            return;

        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_tex.get(),
                                                      m_sampler.get()),
        });
        if (!m_srb->create())
            return;

        m_pipeline.reset(m_rhi->newGraphicsPipeline());
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setShaderStages(
            {{QRhiShaderStage::Vertex,
              QShader::fromSerialized(QByteArray::fromRawData(reinterpret_cast<const char*>(present_vert_qsb),
                                                             int(present_vert_qsb_len)))},
             {QRhiShaderStage::Fragment,
              QShader::fromSerialized(QByteArray::fromRawData(reinterpret_cast<const char*>(present_frag_qsb),
                                                             int(present_frag_qsb_len)))}});

        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({{4 * sizeof(float)}});
        inputLayout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
        });
        m_pipeline->setVertexInputLayout(inputLayout);
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(m_rp.get());
        m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_pipeline->setDepthTest(false);
        m_pipeline->setDepthWrite(false);
        if (!m_pipeline->create())
        {
            m_pipeline.reset();
            return;
        }
        m_pipelineBuilt = true;
    }

    void VulkanPresentWindow::renderFrame()
    {
        if (!m_rhi || !isExposed() || size().isEmpty())
            return;
        if (!m_sc || !m_sc->currentPixelSize().isValid())
        {
            if (!ensureSwapChain())
                return;
        }
        else
        {
            // Recreate when the window pixel size changes (HiDPI / resize).
            const QSize want = QSize(qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio()));
            if (m_sc->currentPixelSize() != want)
            {
                if (!ensureSwapChain())
                    return;
            }
        }

        QRhi::FrameOpResult r = m_rhi->beginFrame(m_sc.get());
        if (r == QRhi::FrameOpSwapChainOutOfDate)
        {
            ensureSwapChain();
            return;
        }
        if (r != QRhi::FrameOpSuccess)
            return;

        QRhiCommandBuffer* cb = m_sc->currentFrameCommandBuffer();
        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();

        if (!m_vbufUploaded && m_vbuf)
        {
            u->uploadStaticBuffer(m_vbuf.get(), kQuad);
            m_vbufUploaded = true;
        }

        // Clip-space correction so NDC Y-up quad is correct on Vulkan.
        {
            QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
            // QMatrix4x4 is column-major; std140 mat4 matches.
            float mat[16];
            memcpy(mat, corr.constData(), sizeof(mat));
            u->updateDynamicBuffer(m_ubuf.get(), 0, 64, mat);
        }

        const QSize outputSize = m_sc->currentPixelSize();

        if (m_hasPending && !m_pending.isNull())
        {
            // GL FBO (grab) and the embedded QWindow swapchain can disagree on
            // fractional DPR (e.g. FBO @2.0 vs window @1.25). Fit the full grab
            // into the swapchain (letterbox if needed).
            QImage img = m_pending;
            if (outputSize.isValid() && img.size() != outputSize)
            {
                img = img.scaled(outputSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (img.size() != outputSize)
                {
                    QImage canvas(outputSize, QImage::Format_RGBA8888);
                    canvas.fill(Qt::black);
                    const int x = (outputSize.width() - img.width()) / 2;
                    const int y = (outputSize.height() - img.height()) / 2;
                    QPainter paint(&canvas);
                    paint.drawImage(x, y, img);
                    paint.end();
                    img = std::move(canvas);
                }
            }

            // Optional synthetic PQ wedges to validate the HDR10 surface itself
            // (bypass GL). Left≈100 nits, center≈1000, right≈400.
            if (getenv("RV_HDR_TEST_PATTERN") && *getenv("RV_HDR_TEST_PATTERN") != '0' && m_hdr)
            {
                img = QImage(outputSize, QImage::Format_RGBA8888);
                // PQ codes for 100 / 1000 / 400 nits (approx 8-bit)
                const int pq100 = 130, pq400 = 164, pq1000 = 192;
                for (int y = 0; y < img.height(); ++y)
                {
                    uchar* row = img.scanLine(y);
                    for (int x = 0; x < img.width(); ++x)
                    {
                        const float fx = float(x) / float(std::max(1, img.width() - 1));
                        int v = pq100;
                        if (fx > 0.66f)
                            v = pq400;
                        else if (fx > 0.33f)
                            v = pq1000;
                        row[x * 4 + 0] = uchar(v);
                        row[x * 4 + 1] = uchar(v);
                        row[x * 4 + 2] = uchar(v);
                        row[x * 4 + 3] = 255;
                    }
                }
            }

            ensureTexture(img.size());
            if (m_tex)
                u->uploadTexture(m_tex.get(), img);
            m_hasPending = false;

            static int s_log = 0;
            if (s_log++ < 5)
            {
                // Sample present tex L/C/R before upload
                auto samp = [&](float fx) {
                    const int x = int(fx * (img.width() - 1));
                    const int y = img.height() / 2;
                    return int(img.pixelColor(x, y).red());
                };
                cout << "INFO: present upload src=" << m_pending.width() << "x" << m_pending.height()
                     << " -> tex=" << img.width() << "x" << img.height() << " swap=" << outputSize.width() << "x"
                     << outputSize.height() << " win=" << width() << "x" << height() << "@" << devicePixelRatio()
                     << " LCR8=" << samp(0.2f) << "," << samp(0.5f) << "," << samp(0.8f)
                     << " scFmt=" << m_swapchainFormat << endl;
            }
        }
        ensurePipeline();

        const QColor clear = Qt::black;
        cb->beginPass(m_sc->currentFrameRenderTarget(), clear, {1.0f, 0}, u);
        if (m_pipeline && m_srb && m_tex && m_vbuf)
        {
            cb->setGraphicsPipeline(m_pipeline.get());
            cb->setViewport(QRhiViewport(0, 0, float(outputSize.width()), float(outputSize.height())));
            cb->setShaderResources(m_srb.get());
            const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);
            cb->setVertexInput(0, 1, &vbufBinding);
            cb->draw(4);
        }
        cb->endPass();
        m_rhi->endFrame(m_sc.get());
    }

    void VulkanPresentWindow::exposeEvent(QExposeEvent*)
    {
        if (isExposed())
        {
            if (!m_rhi)
                initRhi();
            if (m_rhi)
            {
                ensureSwapChain();
                m_running = true;
                renderFrame();
            }
        }
    }

    void VulkanPresentWindow::resizeEvent(QResizeEvent*)
    {
        if (isExposed() && m_rhi)
            ensureSwapChain();
    }

    bool VulkanPresentWindow::event(QEvent* e)
    {
        switch (e->type())
        {
        case QEvent::UpdateRequest:
            renderFrame();
            return true;
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            {
                releaseRhi();
            }
            break;
        default:
            break;
        }
        return QWindow::event(e);
    }

    // -------------------------------------------------------------------------
    // VulkanPresentWidget — embeds the window into the main RV hierarchy
    // -------------------------------------------------------------------------

    VulkanPresentWidget::VulkanPresentWidget(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setFocusPolicy(Qt::NoFocus);

        m_window = new VulkanPresentWindow;
        // Embed as subsurface of this widget → one Hyprland top-level (RV).
        m_container = QWidget::createWindowContainer(m_window, this);
        m_container->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(m_container);
    }

    VulkanPresentWidget::~VulkanPresentWidget()
    {
        m_window = nullptr;
        m_container = nullptr;
    }

    void VulkanPresentWidget::setFrame(QImage img)
    {
        if (m_window)
            m_window->setFrame(std::move(img));
    }

    void VulkanPresentWidget::setHdrPresent(bool enabled)
    {
        if (m_window)
            m_window->setHdrPresent(enabled);
    }

    bool VulkanPresentWidget::hdrPresent() const { return m_window ? m_window->hdrPresent() : false; }

} // namespace Rv
