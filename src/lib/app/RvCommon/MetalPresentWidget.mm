//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <RvCommon/MetalPresentWidget.h>

#include <rhi/qrhi.h>

#include <QEvent>
#include <QExposeEvent>
#include <QMatrix4x4>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#include "shaders/present_vert_qsb.h"
#include "shaders/present_frag_qsb.h"

namespace Rv
{
    using namespace std;

    namespace
    {
        // NDC Y-up quad with UVs for a top-left origin image, matching the
        // layout the shared present shaders expect.
        static const float kQuad[] = {
            // x, y, u, v
            -1.f, -1.f, 0.f, 1.f, //
            1.f,  -1.f, 1.f, 1.f, //
            -1.f, 1.f,  0.f, 0.f, //
            1.f,  1.f,  1.f, 0.f,
        };

        float envFloat(const char* name, float dflt)
        {
            const char* v = getenv(name);
            if (!v || !*v)
                return dflt;
            char* end = nullptr;
            const float f = strtof(v, &end);
            return (end && end != v) ? f : dflt;
        }

        bool envOn(const char* name, bool dflt)
        {
            const char* v = getenv(name);
            if (!v || !*v)
                return dflt;
            return strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "off")
                   && strcasecmp(v, "no");
        }
    } // namespace

    //--------------------------------------------------------------------------
    // MetalPresentWindow
    //--------------------------------------------------------------------------

    MetalPresentWindow::MetalPresentWindow()
    {
        setSurfaceType(QSurface::MetalSurface);
    }

    MetalPresentWindow::~MetalPresentWindow() { releaseRhi(); }

    void MetalPresentWindow::setHdrPresent(bool enabled)
    {
        m_hdr = enabled;
        requestUpdate();
    }

    void MetalPresentWindow::setFrame(QImage img)
    {
        m_pending = std::move(img);
        m_hasPending = true;
        m_hasPendingHalf = false;
        requestUpdate();
    }

    void MetalPresentWindow::setFrameHalf(int width, int height, std::vector<uint16_t> rgba16)
    {
        m_pendingHalf = std::move(rgba16);
        m_pendingHalfW = width;
        m_pendingHalfH = height;
        m_hasPendingHalf = true;
        m_hasPending = false;
        requestUpdate();
    }

    void MetalPresentWindow::refreshEdrInfo()
    {
        @autoreleasepool
        {
            NSScreen* screen = nil;
            if (WId wid = winId())
            {
                id obj = reinterpret_cast<id>(wid);
                if ([obj isKindOfClass:[NSView class]])
                    screen = static_cast<NSView*>(obj).window.screen;
            }
            if (!screen)
                screen = [NSScreen mainScreen];
            if (screen)
                m_edrHeadroom =
                    float(screen.maximumExtendedDynamicRangeColorComponentValue);
        }
    }

    void MetalPresentWindow::exposeEvent(QExposeEvent*)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            cout << "INFO: MetalPresentWindow: first exposeEvent, isExposed="
                 << (isExposed() ? "yes" : "no") << " size=" << width() << "x" << height()
                 << endl;
        }
        if (isExposed())
        {
            initRhi();
            renderFrame();
        }
    }

    void MetalPresentWindow::resizeEvent(QResizeEvent*)
    {
        if (isExposed() && m_sc)
            renderFrame();
    }

    bool MetalPresentWindow::event(QEvent* e)
    {
        if (e->type() == QEvent::UpdateRequest)
        {
            renderFrame();
        }
        else if (e->type() == QEvent::PlatformSurface
                 && static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
                        == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        {
            releaseRhi();
        }
        return QWindow::event(e);
    }

    void MetalPresentWindow::initRhi()
    {
        if (m_rhi)
            return;

        QRhiMetalInitParams params;
        m_rhi = QRhi::create(QRhi::Metal, &params);
        if (!m_rhi)
        {
            cerr << "ERROR: MetalPresentWindow: QRhi::create(Metal) failed" << endl;
            return;
        }

        if (!ensureSwapChain())
            return;

        m_running = true;
        refreshEdrInfo();

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            cout << "INFO: Metal present surface: "
                 << (m_edrSurface ? "HDRExtendedDisplayP3Linear (EDR)" : "SDR")
                 << ", headroom=" << m_edrHeadroom << endl;
        }
    }

    bool MetalPresentWindow::ensureSwapChain()
    {
        if (!m_rhi)
            return false;

        if (!m_sc)
        {
            m_sc.reset(m_rhi->newSwapChain());
            m_sc->setWindow(this);

            // Qt maps this to RGBA16F + kCGColorSpaceExtendedLinearDisplayP3 +
            // wantsExtendedDynamicRangeContent. Note Qt's Metal backend exposes
            // no HDR10/PQ swapchain format; extended-linear is the EDR route.
            const QRhiSwapChain::Format wanted = QRhiSwapChain::HDRExtendedDisplayP3Linear;
            if (m_hdr && envOn("RV_MACOS_EDR", true) && m_sc->isFormatSupported(wanted))
            {
                m_sc->setFormat(wanted);
                m_edrSurface = true;
            }
            else
            {
                m_edrSurface = false;
                if (m_hdr)
                    cerr << "WARNING: MetalPresentWindow: EDR surface unavailable; using SDR"
                         << endl;
            }

            m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
            m_sc->setRenderPassDescriptor(m_rp.get());
        }

        m_sc->createOrResize();
        return true;
    }

    void MetalPresentWindow::ensureTexture(const QSize& pixelSize, bool asFloat)
    {
        if (!m_rhi || !pixelSize.isValid())
            return;
        if (m_tex && m_texSize == pixelSize && m_texIsFloat == asFloat)
            return;

        const QRhiTexture::Format fmt = asFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
        m_tex.reset(m_rhi->newTexture(fmt, pixelSize));
        if (!m_tex->create())
        {
            cerr << "ERROR: MetalPresentWindow: texture create failed" << endl;
            m_tex.reset();
            return;
        }
        m_texSize = pixelSize;
        m_texIsFloat = asFloat;
        m_pipelineBuilt = false; // srb references the texture
    }

    void MetalPresentWindow::ensurePipeline()
    {
        if (!m_rhi || m_pipelineBuilt || !m_tex)
            return;

        if (!m_sampler)
        {
            m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                              QRhiSampler::None, QRhiSampler::ClampToEdge,
                                              QRhiSampler::ClampToEdge));
            m_sampler->create();
        }

        if (!m_vbuf)
        {
            m_vbuf.reset(
                m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
            m_vbuf->create();
            m_vbufUploaded = false;
        }

        if (!m_ubuf)
        {
            // mat4 (64) + vec4 params (16)
            m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
            m_ubuf->create();
        }

        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0,
                                                     QRhiShaderResourceBinding::VertexStage
                                                         | QRhiShaderResourceBinding::FragmentStage,
                                                     m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_tex.get(), m_sampler.get()),
        });
        if (!m_srb->create())
            return;

        // The shared present shaders already carry an MSL 12 variant, so the
        // same .qsb blobs the Vulkan backend uses work unchanged here.
        m_pipeline.reset(m_rhi->newGraphicsPipeline());
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setShaderStages(
            {{QRhiShaderStage::Vertex,
              QShader::fromSerialized(QByteArray::fromRawData(
                  reinterpret_cast<const char*>(present_vert_qsb), int(present_vert_qsb_len)))},
             {QRhiShaderStage::Fragment,
              QShader::fromSerialized(QByteArray::fromRawData(
                  reinterpret_cast<const char*>(present_frag_qsb), int(present_frag_qsb_len)))}});

        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({{4 * sizeof(float)}});
        inputLayout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
        });
        m_pipeline->setVertexInputLayout(inputLayout);
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(m_rp.get());
        if (!m_pipeline->create())
        {
            cerr << "ERROR: MetalPresentWindow: pipeline create failed" << endl;
            return;
        }
        m_pipelineBuilt = true;
    }

    void MetalPresentWindow::renderFrame()
    {
        if (!m_running || !m_rhi || !m_sc || !isExposed())
            return;

        if (m_sc->currentPixelSize() != m_sc->surfacePixelSize())
            m_sc->createOrResize();

        if (m_rhi->beginFrame(m_sc.get()) != QRhi::FrameOpSuccess)
            return;

        refreshEdrInfo();

        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();

        if (!m_vbufUploaded && m_vbuf)
        {
            u->uploadStaticBuffer(m_vbuf.get(), kQuad);
            m_vbufUploaded = true;
        }

        QSize texSize = m_texSize;
        bool haveTex = m_tex != nullptr;

        if (m_hasPendingHalf && !m_pendingHalf.empty())
        {
            texSize = QSize(m_pendingHalfW, m_pendingHalfH);
            ensureTexture(texSize, true);
            if (m_tex)
            {
                QByteArray bytes(reinterpret_cast<const char*>(m_pendingHalf.data()),
                                 qsizetype(m_pendingHalf.size() * sizeof(uint16_t)));
                QRhiTextureSubresourceUploadDescription sub(bytes);
                sub.setSourceSize(texSize);
                QRhiTextureUploadEntry entry(0, 0, sub);
                u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription(entry));
                haveTex = true;
                // Raw GL float FBO readback is bottom-up.
                m_flipV = 1.f;
            }
            m_hasPendingHalf = false;
        }
        else if (m_hasPending && !m_pending.isNull())
        {
            QImage img = m_pending.convertToFormat(QImage::Format_RGBA8888);
            texSize = img.size();
            ensureTexture(texSize, false);
            if (m_tex)
            {
                u->uploadTexture(m_tex.get(), img);
                haveTex = true;
                // grabFramebuffer() already returns a top-left origin QImage.
                m_flipV = 0.f;
            }
            m_hasPending = false;
        }

        ensurePipeline();

        if (m_ubuf)
        {
            QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
            alignas(16) float ubo[20];
            memcpy(ubo, corr.constData(), 64);

            // Decoder must match what DisplayIPNode encoded into the buffer.
            //
            // Default on macOS is Display P3 Extended: P3 primaries with the
            // piecewise sRGB transfer, values free to exceed 1.0. Mode 3 is its
            // decoder (sRGB EOTF -> linear P3), and it lands directly on an
            // extended-linear Display P3 surface where 1.0 is SDR white.
            //
            // With RV_HDR_ENCODING=pq the buffer holds SMPTE-2084 codes instead
            // and mode 2 is required (PQ -> nits -> /sdrWhite). Note Qt's Metal
            // backend has no HDR10 swapchain, so PQ on macOS still has to be
            // decoded to extended-linear here rather than presented as PQ.
            int mode = 0;
            if (m_hdr && m_edrSurface)
            {
                const char* enc = getenv("RV_HDR_ENCODING");
                const bool pq = enc && *enc
                                && (!strcasecmp(enc, "pq") || !strcasecmp(enc, "st2084"));
                mode = int(envFloat("RV_MACOS_PRESENT_MODE", pq ? 2.f : 3.f));
            }

            // Nits that should land on 1.0 (SDR white). 203 is the usual
            // graphics/reference white for PQ mastering and matches the
            // Linux backend's default.
            const float sdrWhite = envFloat("RV_HDR_SDR_WHITE", 203.f);

            // No Wayland-style white-match boost here: on an extended-linear
            // macOS surface linear 1.0 already is SDR white.
            ubo[16] = 1.0f;
            ubo[17] = float(mode);
            ubo[18] = sdrWhite;
            ubo[19] = m_flipV;
            u->updateDynamicBuffer(m_ubuf.get(), 0, 80, ubo);

            static int loggedMode = -1;
            if (loggedMode != mode)
            {
                loggedMode = mode;
                cout << "INFO: Metal present decode mode=" << mode
                     << (mode == 2   ? " (PQ codes -> linear, 1.0 = sdrWhite)"
                         : mode == 3 ? " (piecewise sRGB EOTF -> linear P3)"
                                     : " (passthrough)")
                     << " sdrWhite=" << sdrWhite << " nits, flipV=" << m_flipV << endl;
            }
        }

        if (!m_pipelineBuilt)
        {
            // Nothing to draw yet; still end the frame so the swapchain advances.
            QRhiCommandBuffer* cb = m_sc->currentFrameCommandBuffer();
            cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0), {1.0f, 0}, u);
            cb->endPass();
            m_rhi->endFrame(m_sc.get());
            return;
        }

        QRhiCommandBuffer* cb = m_sc->currentFrameCommandBuffer();
        const QSize outputSize = m_sc->currentPixelSize();

        // Aspect-preserving fit, matching the Vulkan backend.
        float vpX = 0.f, vpY = 0.f;
        float vpW = float(outputSize.width()), vpH = float(outputSize.height());
        if (haveTex && texSize.isValid() && texSize.width() > 0 && texSize.height() > 0)
        {
            const float s = std::min(float(outputSize.width()) / float(texSize.width()),
                                     float(outputSize.height()) / float(texSize.height()));
            vpW = float(texSize.width()) * s;
            vpH = float(texSize.height()) * s;
            vpX = 0.5f * (float(outputSize.width()) - vpW);
            vpY = 0.5f * (float(outputSize.height()) - vpH);
        }

        cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0), {1.0f, 0}, u);
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport({vpX, vpY, vpW, vpH});
        cb->setShaderResources(m_srb.get());
        const QRhiCommandBuffer::VertexInput vb(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &vb);
        cb->draw(4);
        cb->endPass();

        m_rhi->endFrame(m_sc.get());
    }

    void MetalPresentWindow::releaseRhi()
    {
        m_running = false;
        m_pipeline.reset();
        m_srb.reset();
        m_ubuf.reset();
        m_vbuf.reset();
        m_sampler.reset();
        m_tex.reset();
        m_rp.reset();
        m_sc.reset();
        delete m_rhi;
        m_rhi = nullptr;
        m_pipelineBuilt = false;
        m_vbufUploaded = false;
    }

    //--------------------------------------------------------------------------
    // MetalPresentWidget
    //--------------------------------------------------------------------------

    MetalPresentWidget::MetalPresentWidget(QWidget* parent)
        : QWidget(parent)
    {
        // Match VulkanPresentWidget: the overlay must never take input away
        // from the image area (pan, grade, scrub all live in GLView).
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setFocusPolicy(Qt::NoFocus);

        m_window = new MetalPresentWindow;
        m_container = QWidget::createWindowContainer(m_window, this);
        m_container->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_container->setFocusPolicy(Qt::NoFocus);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_container);

        if (getenv("RV_METAL_PRESENT_DEBUG"))
        {
            QTimer::singleShot(3000, this, [this] {
                cout << "DEBUG: MetalPresentWidget"
                     << " widget visible=" << isVisible() << " size=" << width() << "x"
                     << height() << " | container visible=" << m_container->isVisible()
                     << " size=" << m_container->width() << "x" << m_container->height()
                     << " | window visible=" << m_window->isVisible()
                     << " exposed=" << m_window->isExposed() << " size=" << m_window->width()
                     << "x" << m_window->height() << endl;
            });
        }
    }

    MetalPresentWidget::~MetalPresentWidget() = default;

    void MetalPresentWidget::setFrame(QImage img) { m_window->setFrame(std::move(img)); }

    void MetalPresentWidget::setFrameHalf(int width, int height, std::vector<uint16_t> rgba16)
    {
        m_window->setFrameHalf(width, height, std::move(rgba16));
    }

    void MetalPresentWidget::setHdrPresent(bool enabled) { m_window->setHdrPresent(enabled); }

    bool MetalPresentWidget::hdrPresent() const { return m_window->hdrPresent(); }

    bool MetalPresentWidget::needsFloatTransfer() const { return m_window->hdrPresent(); }

} // namespace Rv
