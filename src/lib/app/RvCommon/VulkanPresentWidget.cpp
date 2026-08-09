//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <RvCommon/VulkanPresentWidget.h>
#include <RvCommon/GlVkSharedImage.h>

// Vulkan headers must be visible before qrhi_platform.h enables QRhiVulkanInitParams.
#include <vulkan/vulkan.h>

#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <rhi/qrhi_platform.h>

#include <QOpenGLContext>

#include <QColorSpace>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QSurfaceFormat>
#include <QVulkanInstance>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <strings.h> // strcasecmp

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

        // RV_HDR_PQ_SDR_SCALE=1 enables PQ→linear→×s→PQ on the PQ present path.
        bool wantPqSdrWhiteScale()
        {
            const char* e = getenv("RV_HDR_PQ_SDR_SCALE");
            if (!e || !*e)
                return false;
            if (!strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no"))
                return false;
            return true;
        }

        // White-match for linear present (p3extended). Default ON; set
        // RV_HDR_LINEAR_SDR_MATCH=0 to disable.
        bool wantLinearSdrWhiteMatch()
        {
            const char* e = getenv("RV_HDR_LINEAR_SDR_MATCH");
            if (!e || !*e)
                return true; // default on — same class of Wayland 203-nit issue
            if (!strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no"))
                return false;
            return true;
        }

        float envFloat(const char* name, float fallback)
        {
            const char* e = getenv(name);
            if (!e || !*e)
                return fallback;
            char* end = nullptr;
            const float v = std::strtof(e, &end);
            if (end == e)
                return fallback;
            return v;
        }

    } // namespace

    // -------------------------------------------------------------------------
    // VulkanPresentWindow
    // -------------------------------------------------------------------------

    VulkanPresentWindow::VulkanPresentWindow()
        : QWindow()
    {
        setSurfaceType(QSurface::VulkanSurface);
        // Present-only surface: all clicks/drags must reach GLView underneath
        // (pan, E+drag grade, playhead, etc.). Native windows ignore
        // WA_TransparentForMouseEvents on the QWidget container.
        setFlags(flags() | Qt::WindowTransparentForInput);
    }

    VulkanPresentWindow::~VulkanPresentWindow() { releaseRhi(); }

    VulkanPresentWindow::PresentMode VulkanPresentWindow::presentModeFromEnv()
    {
        const char* e = getenv("RV_HDR_PRESENT");
        if (!e || !*e)
            return PresentMode::Pq;
        // Accept a few aliases for A/B testing.
        if (!strcasecmp(e, "p3extended") || !strcasecmp(e, "p3-extended") || !strcasecmp(e, "p3_extended")
            || !strcasecmp(e, "displayp3-extended") || !strcasecmp(e, "display-p3-extended")
            || !strcasecmp(e, "edr-p3") || !strcasecmp(e, "edrp3") || !strcasecmp(e, "p3-srgb")
            || !strcasecmp(e, "p3srgb"))
            return PresentMode::P3Extended;
        if (!strcasecmp(e, "p3") || !strcasecmp(e, "p3linear") || !strcasecmp(e, "linear-p3")
            || !strcasecmp(e, "linear_p3") || !strcasecmp(e, "p3-linear"))
            return PresentMode::P3Linear;
        if (!strcasecmp(e, "scrgb") || !strcasecmp(e, "srgb") || !strcasecmp(e, "srgblinear")
            || !strcasecmp(e, "linear-srgb") || !strcasecmp(e, "srgb-linear"))
            return PresentMode::SrgbLinear;
        if (!strcasecmp(e, "pq") || !strcasecmp(e, "hdr10") || !strcasecmp(e, "st2084"))
            return PresentMode::Pq;
        cerr << "WARNING: unknown RV_HDR_PRESENT=" << e
             << " (use pq|p3linear|srgblinear|p3extended); defaulting to pq" << endl;
        return PresentMode::Pq;
    }

    void VulkanPresentWindow::setHdrPresent(bool enabled)
    {
        m_hdr = enabled;
        m_presentMode = presentModeFromEnv();
        QSurfaceFormat fmt = format();
        if (!m_hdr)
        {
            fmt.setColorSpace(QColorSpace(QColorSpace::SRgb));
        }
        else if (m_presentMode == PresentMode::P3Linear || m_presentMode == PresentMode::P3Extended)
        {
            // Must tag LINEAR transfer. QColorSpace::DisplayP3 is *gamma* (sRGB TF);
            // if we emit linear light but advertise gamma P3, the compositor applies
            // EOTF again → double-decode (too dark / "wrong sRGB→linear").
            // P3Extended: OCIO Display P3 buffer is sRGB-TF encoded; shader EOTFs.
            // P3Linear: buffer is PQ codes; shader maps nits/sdrWhite → linear.
            fmt.setColorSpace(
                QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear));
        }
        else if (m_presentMode == PresentMode::SrgbLinear)
        {
            fmt.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        }
        else
        {
            fmt.setColorSpace(QColorSpace(QColorSpace::Bt2100Pq));
        }
        setFormat(fmt);

        const char* modeName = "pq/HDR10";
        if (m_presentMode == PresentMode::P3Linear)
            modeName = "p3linear";
        else if (m_presentMode == PresentMode::P3Extended)
            modeName = "p3extended (DisplayP3 + sRGB TF → linear P3)";
        else if (m_presentMode == PresentMode::SrgbLinear)
            modeName = "srgblinear";
        cout << "INFO: VulkanPresentWindow HDR present " << (enabled ? "ON" : "OFF")
             << " mode=" << modeName << " (RV_HDR_PRESENT)" << endl;

        // Force swapchain re-pick on next frame if already live.
        m_swapchainFormat = -1;
        m_pipelineBuilt = false;
    }

    bool VulkanPresentWindow::presentModeNeedsFloatTransfer()
    {
        return presentModeFromEnv() == PresentMode::P3Extended;
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
        m_hasPendingHalf = false;
        m_pendingHalf.clear();
        if (isExposed())
            requestUpdate();
    }

    void VulkanPresentWindow::setFrameHalf(int width, int height, std::vector<uint16_t> rgba16)
    {
        if (width <= 0 || height <= 0 || rgba16.size() < size_t(width) * size_t(height) * 4)
            return;
        m_pendingHalf = std::move(rgba16);
        m_pendingHalfW = width;
        m_pendingHalfH = height;
        m_hasPendingHalf = true;
        m_hasPending = false;
        m_pending = QImage();
        if (isExposed())
            requestUpdate();
    }

    void VulkanPresentWindow::initRhi()
    {
        if (m_rhi)
            return;

        // QRhi needs a realized platform window to pick a present-capable queue.
        // ensureGpuInterop can run from GL paint before first expose — refuse early
        // so we don't burn a failed create and leave a half-init instance.
        if (!handle() || !isExposed())
            return;

        if (!m_inst)
        {
            m_inst = new QVulkanInstance;
            // External-memory capabilities are an instance extension on some stacks.
            QByteArrayList instExt = QRhiVulkanInitParams::preferredInstanceExtensions();
            if (!instExt.contains("VK_KHR_external_memory_capabilities"))
                instExt.append("VK_KHR_external_memory_capabilities");
            if (!instExt.contains("VK_KHR_get_physical_device_properties2"))
                instExt.append("VK_KHR_get_physical_device_properties2");
            m_inst->setExtensions(instExt);
            if (!m_inst->create())
            {
                cerr << "ERROR: QVulkanInstance::create failed" << endl;
                delete m_inst;
                m_inst = nullptr;
                return;
            }
            setVulkanInstance(m_inst);
        }

        QRhiVulkanInitParams params;
        params.inst = m_inst;
        params.window = this;
        // Enable external memory so we can share color images with OpenGL (no readback).
        params.deviceExtensions = GlVkSharedImage::requiredDeviceExtensions();
        m_rhi = QRhi::create(QRhi::Vulkan, &params);
        if (!m_rhi)
        {
            cerr << "WARNING: QRhi Vulkan with external-memory extensions failed; retrying without"
                 << endl;
            params.deviceExtensions.clear();
            m_rhi = QRhi::create(QRhi::Vulkan, &params);
        }
        if (!m_rhi)
        {
            cerr << "ERROR: QRhi::create(Vulkan) failed" << endl;
            return;
        }
        cout << "INFO: VulkanPresentWindow RHI backend=" << m_rhi->backendName()
             << " platform=" << QGuiApplication::platformName().toStdString()
             << " dpr=" << devicePixelRatio() << " size=" << width() << "x" << height()
             << " yUpNDC=" << m_rhi->isYUpInNDC() << " yUpFB=" << m_rhi->isYUpInFramebuffer()
             << " extMem=" << (params.deviceExtensions.isEmpty() ? "no" : "yes") << endl;

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
        // std140: mat4 (64) + vec4 params (16) = 80. params.x = PQ SDR-white scale.
        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
        m_ubuf->create();
    }

    void VulkanPresentWindow::selectSwapChainFormat()
    {
        if (!m_sc)
            return;

        // Re-read env each recreate so A/B doesn't require full restart of RHI
        // object graph (still needs window recreate for surface colorspace).
        if (m_hdr)
            m_presentMode = presentModeFromEnv();

        QRhiSwapChain::Format fmt = QRhiSwapChain::SDR;
        const char* name = "SDR";

        auto tryFmt = [&](QRhiSwapChain::Format f, const char* n) -> bool {
            if (!m_sc->isFormatSupported(f))
                return false;
            fmt = f;
            name = n;
            return true;
        };

        if (m_hdr)
        {
            bool ok = false;
            if (m_presentMode == PresentMode::P3Linear || m_presentMode == PresentMode::P3Extended)
            {
                ok = tryFmt(QRhiSwapChain::HDRExtendedDisplayP3Linear, "HDRExtendedDisplayP3Linear");
                if (!ok)
                    cout << "WARNING: P3 linear HDR unsupported by QRhi/Vulkan here" << endl;
            }
            else if (m_presentMode == PresentMode::SrgbLinear)
            {
                ok = tryFmt(QRhiSwapChain::HDRExtendedSrgbLinear, "HDRExtendedSrgbLinear (scRGB)");
                if (!ok)
                    cout << "WARNING: scRGB linear HDR unsupported by QRhi/Vulkan here" << endl;
            }
            else
            {
                ok = tryFmt(QRhiSwapChain::HDR10, "HDR10 (PQ/ST.2084)");
            }

            // Fallbacks so we still get *some* HDR surface for comparison.
            if (!ok)
                ok = tryFmt(QRhiSwapChain::HDR10, "HDR10 (PQ/ST.2084) [fallback]");
            if (!ok)
                ok = tryFmt(QRhiSwapChain::HDRExtendedSrgbLinear, "HDRExtendedSrgbLinear [fallback]");
            if (!ok)
                ok = tryFmt(QRhiSwapChain::HDRExtendedDisplayP3Linear, "HDRExtendedDisplayP3Linear [fallback]");
            if (!ok)
            {
                cout << "WARNING: no HDR swapchain format supported — presenting as SDR" << endl;
            }
        }

        m_sc->setFormat(fmt);
        if (int(fmt) != m_swapchainFormat)
        {
            m_swapchainFormat = int(fmt);
            cout << "INFO: VulkanPresentWindow swapchain format=" << name
                 << " presentMode=" << int(m_presentMode) << endl;
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
        if (m_shared)
            m_shared->destroy();
        m_shared.reset();
        m_gpuInterop = false;
        m_sharedDirty = false;
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

    bool VulkanPresentWindow::usingGpuInterop() const { return m_gpuInterop && m_shared && m_shared->valid(); }

    bool VulkanPresentWindow::ensureGpuInterop(QOpenGLContext* glctx, const QSize& pixelSize, bool float16)
    {
        // Do not init RHI from the GL paint path before the present window is
        // exposed — that fails queue selection and can poison startup. Wait for
        // exposeEvent to create QRhi, then flip to interop on a later frame.
        if (!m_rhi)
            return false;
        if (!glctx || !pixelSize.isValid())
            return false;
        // Sticky disable after a failed create/blit — avoid hammering every frame.
        static bool s_interopFailed = false;
        if (s_interopFailed)
        {
            m_gpuInterop = false;
            return false;
        }

        if (!GlVkSharedImage::isSupported(m_rhi, glctx))
        {
            static bool once = false;
            if (!once)
            {
                once = true;
                cout << "INFO: present uses CPU readback (GPU interop unavailable; "
                        "RV_HDR_GL_VK_INTEROP=0 forces this)"
                     << endl;
            }
            m_gpuInterop = false;
            return false;
        }
        if (!m_shared)
            m_shared = std::make_unique<GlVkSharedImage>();
        if (!m_shared->create(m_rhi, glctx, pixelSize, float16))
        {
            static bool onceFail = false;
            if (!onceFail)
            {
                onceFail = true;
                cout << "INFO: GL↔Vulkan shared image create failed; CPU readback present" << endl;
            }
            s_interopFailed = true;
            m_gpuInterop = false;
            return false;
        }
        // Sample source is m_shared->sampleTexture() (separate VkImage, GPU-copied).
        // Drop the CPU-upload texture so the pipeline binds the sample image.
        m_tex.reset();
        m_texIsFloat = float16;
        m_texSize = pixelSize;
        m_pipelineBuilt = false;
        m_srb.reset();
        m_pipeline.reset();
        m_gpuInterop = true;
        return true;
    }

    bool VulkanPresentWindow::blitFromGlFramebuffer(unsigned int srcFbo, int width, int height)
    {
        if (!usingGpuInterop())
            return false;
        if (!m_shared->blitFromFramebuffer(GLuint(srcFbo), width, height))
        {
            // Shared blit produced no pixels — permanently fall back to CPU path.
            static bool once = false;
            if (!once)
            {
                once = true;
                cout << "INFO: disabling GPU interop after failed shared blit; CPU readback present"
                     << endl;
            }
            m_gpuInterop = false;
            if (m_shared)
                m_shared->destroy();
            return false;
        }
        m_sharedDirty = true;
        return true;
    }

    void VulkanPresentWindow::presentGpuInteropFrame()
    {
        if (!usingGpuInterop() || !m_rhi || !isExposed())
            return;
        m_hasPending = false;
        m_hasPendingHalf = false;
        // GL has blitted into the shared image and glFinish'd. GPU-copy into the
        // QRhi-owned texture and present (shared image never leaves GENERAL for GL).
        renderFrame();
        m_sharedDirty = false;
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

        // Always refresh SDR white from the swapchain when HDR is on — this is
        // Compositor/Qt idea of "SDR 1.0" in nits (fallback default is 203).
        if (m_hdr)
        {
            const QRhiSwapChainHdrInfo hi = m_sc->hdrInfo();
            if (hi.sdrWhiteLevel > 1.f)
                m_sdrWhiteLevelNits = float(hi.sdrWhiteLevel);

            static bool loggedHdrInfo = false;
            if (!loggedHdrInfo)
            {
                loggedHdrInfo = true;
                cout << "INFO: swapchain HDR info: limitsType=" << int(hi.limitsType)
                     << " sdrWhiteLevel=" << hi.sdrWhiteLevel;
                if (hi.limitsType == QRhiSwapChainHdrInfo::LuminanceInNits)
                {
                    cout << " minNits=" << hi.limits.luminanceInNits.minLuminance
                         << " maxNits=" << hi.limits.luminanceInNits.maxLuminance;
                }
                cout << " formatEnum=" << int(m_sc->format()) << endl;
            }
        }

        m_pipeline.reset();
        m_pipelineBuilt = false;
        return true;
    }

    void VulkanPresentWindow::ensureTexture(const QSize& pixelSize, bool asFloat)
    {
        if (!m_rhi || !pixelSize.isValid())
            return;
        if (m_tex && m_texSize == pixelSize && m_texIsFloat == asFloat)
            return;
        m_tex.reset();
        // p3extended: GL FBO is RGBA16F; CPU path reads GL_HALF_FLOAT and uploads here.
        // Half matches the render target and is ~½ the PCIe cost of float32.
        const QRhiTexture::Format fmt = asFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
        m_tex.reset(m_rhi->newTexture(fmt, pixelSize, 1, {}));
        if (!m_tex->create())
        {
            cerr << "ERROR: present texture create failed (float16=" << asFloat << ")" << endl;
            m_tex.reset();
            m_texIsFloat = false;
            return;
        }
        m_texSize = pixelSize;
        m_texIsFloat = asFloat;
        m_srb.reset();
        m_pipeline.reset();
        m_pipelineBuilt = false;
        if (asFloat)
        {
            static bool once = false;
            if (!once)
            {
                once = true;
                cout << "INFO: present upload texture RGBA16F (half-float transfer for p3extended)" << endl;
            }
        }
    }

    void VulkanPresentWindow::ensurePipeline()
    {
        QRhiTexture* sampleTex = usingGpuInterop() ? m_shared->sampleTexture() : m_tex.get();
        if (!m_rhi || m_pipelineBuilt || !sampleTex || !m_sampler || !m_rp || !m_ubuf)
            return;

        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, sampleTex,
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

        const QSize outputSize = m_sc->currentPixelSize();

        // Letterbox in the GPU viewport — never CPU-scale full frames.
        float flipV = 0.f;
        QSize texSize;
        bool haveTex = false;
        const bool interop = usingGpuInterop();

        if (interop)
        {
            // Shared image already filled by GL blit; GPU-copy into sample image.
            texSize = m_shared->size();
            if (m_shared->copyToSampleTexture())
            {
                haveTex = true;
                flipV = 1.f; // FBO blit preserves GL bottom-up orientation
            }
            else
            {
                static int s_copyFail = 0;
                if (s_copyFail++ < 5)
                    cerr << "ERROR: GPU interop copyToSampleTexture failed" << endl;
            }
            static int s_ilog = 0;
            if (s_ilog++ < 3)
            {
                cout << "INFO: present GPU interop " << texSize.width() << "x" << texSize.height()
                     << " swap=" << outputSize.width() << "x" << outputSize.height()
                     << " shMode=" << presentShaderMode() << " copy=" << (haveTex ? "ok" : "fail")
                     << endl;
            }
        }
        else if (m_hasPendingHalf && !m_pendingHalf.empty())
        {
            // CPU half-float path (p3extended without interop): binary16 × 4 channels.
            const int srcW = m_pendingHalfW;
            const int srcH = m_pendingHalfH;
            texSize = QSize(srcW, srcH);
            ensureTexture(texSize, true);
            if (m_tex)
            {
                QByteArray bytes(reinterpret_cast<const char*>(m_pendingHalf.data()),
                                 qsizetype(m_pendingHalf.size() * sizeof(uint16_t)));
                QRhiTextureSubresourceUploadDescription sub(bytes);
                sub.setSourceSize(texSize);
                QRhiTextureUploadEntry entry(0, 0, sub);
                QRhiTextureUploadDescription desc(entry);
                u->uploadTexture(m_tex.get(), desc);
                haveTex = true;
                flipV = 1.f;
            }
            m_hasPendingHalf = false;
        }
        else if (m_hasPending && !m_pending.isNull())
        {
            QImage img = m_pending;
            if (getenv("RV_HDR_TEST_PATTERN") && *getenv("RV_HDR_TEST_PATTERN") != '0' && m_hdr)
            {
                img = QImage(outputSize.isValid() ? outputSize : img.size(), QImage::Format_RGBA8888);
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

            texSize = img.size();
            ensureTexture(texSize, false);
            if (m_tex)
            {
                u->uploadTexture(m_tex.get(), img);
                haveTex = true;
                flipV = 0.f;
            }
            m_hasPending = false;
        }

        {
            QMatrix4x4 corr = m_rhi->clipSpaceCorrMatrix();
            alignas(16) float ubo[20];
            memcpy(ubo, corr.constData(), 64);
            const int mode = m_hdr ? presentShaderMode() : 0;
            const float sdrWhite = resolveSdrWhiteNits();
            float scale = 1.f;
            if (mode == 1 && wantPqSdrWhiteScale())
                scale = pqSdrWhiteScale();
            else if (mode == 3)
                scale = linearSdrWhiteMatchScale();
            ubo[16] = scale;
            ubo[17] = float(mode);
            ubo[18] = sdrWhite;
            ubo[19] = flipV;
            u->updateDynamicBuffer(m_ubuf.get(), 0, 80, ubo);
        }

        ensurePipeline();

        float vpX = 0.f, vpY = 0.f, vpW = float(outputSize.width()), vpH = float(outputSize.height());
        if (haveTex && texSize.isValid() && outputSize.isValid() && texSize.width() > 0 && texSize.height() > 0)
        {
            const float sx = float(outputSize.width()) / float(texSize.width());
            const float sy = float(outputSize.height()) / float(texSize.height());
            const float s = std::min(sx, sy);
            vpW = float(texSize.width()) * s;
            vpH = float(texSize.height()) * s;
            vpX = 0.5f * (float(outputSize.width()) - vpW);
            vpY = 0.5f * (float(outputSize.height()) - vpH);
        }

        QRhiTexture* sampleTex = interop ? m_shared->sampleTexture() : m_tex.get();
        const QColor clear = Qt::black;
        cb->beginPass(m_sc->currentFrameRenderTarget(), clear, {1.0f, 0}, u);
        if (m_pipeline && m_srb && sampleTex && m_vbuf && haveTex)
        {
            cb->setGraphicsPipeline(m_pipeline.get());
            cb->setViewport(QRhiViewport(vpX, vpY, vpW, vpH));
            cb->setShaderResources(m_srb.get());
            const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);
            cb->setVertexInput(0, 1, &vbufBinding);
            cb->draw(4);
        }
        cb->endPass();
        m_rhi->endFrame(m_sc.get());
    }

    float VulkanPresentWindow::resolveSdrWhiteNits() const
    {
        // Prefer live swapchain value as reported; env override; else media white 203.
        float sdrWhite = m_sdrWhiteLevelNits;
        if (const char* e = getenv("RV_HDR_SDR_WHITE"); e && *e)
            sdrWhite = envFloat("RV_HDR_SDR_WHITE", sdrWhite > 1.f ? sdrWhite : 203.f);
        if (sdrWhite <= 1.f)
            sdrWhite = 203.f;
        return sdrWhite;
    }

    int VulkanPresentWindow::presentShaderMode() const
    {
        // 0 = passthrough
        // 1 = PQ SDR-white scale (stay on HDR10)
        // 2 = PQ → linear (1.0 = sdrWhite) for linear surfaces fed PQ codes
        // 3 = piecewise sRGB TF → linear (Display P3 Extended / macOS EDR)
        if (!m_hdr)
            return 0;

        if (m_presentMode == PresentMode::P3Extended)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                cout << "INFO: present shader mode=DisplayP3 Extended "
                        "(sRGB EOTF → linear P3, float transfer). "
                        "Expect OCIO Display P3 (sRGB TF), not PQ. "
                        "Linear SDR white-match default ON (RV_HDR_LINEAR_SDR_MATCH=0 to disable)."
                     << endl;
            }
            return 3;
        }

        if (m_presentMode == PresentMode::P3Linear || m_presentMode == PresentMode::SrgbLinear)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                cout << "INFO: present shader mode=PQ→linear (1.0=" << resolveSdrWhiteNits()
                     << " nits SDR white) for linear HDR surface. "
                        "Input FBO is still 8-bit PQ codes from OCIO/Display."
                     << endl;
            }
            return 2;
        }

        // PQ / HDR10 path
        if (wantPqSdrWhiteScale())
            return 1;
        return 0;
    }

    float VulkanPresentWindow::pqSdrWhiteScale() const
    {
        const float sdrWhite = resolveSdrWhiteNits();
        // Content / mastering reference white the PQ buffer was authored against.
        const float refWhite = envFloat("RV_HDR_PQ_REF_WHITE", 100.f);
        if (refWhite <= 0.f)
            return 1.f;

        const float scale = sdrWhite / refWhite;

        static bool logged = false;
        if (!logged && wantPqSdrWhiteScale())
        {
            logged = true;
            cout << "INFO: post-OCIO PQ SDR-white scale ON (GPU present): PQ→linear→×" << scale
                 << "→PQ  (sdrWhite=" << sdrWhite << " nits, refWhite=" << refWhite
                 << " nits; set RV_HDR_SDR_WHITE / RV_HDR_PQ_REF_WHITE to override)"
                 << endl;
        }
        return scale;
    }

    float VulkanPresentWindow::linearSdrWhiteMatchScale() const
    {
        if (!wantLinearSdrWhiteMatch())
            return 1.f;

        // After EOTF, content paper white is linear 1.0. On this Wayland stack
        // linear-1.0 often sits below pure SDR UI white — same root cause as the
        // PQ 203-nit issue. Boost by sdrWhite/refWhite (defaults 203/100 ≈ 2.03).
        // Override: RV_HDR_SDR_WHITE, RV_HDR_PQ_REF_WHITE (or set MATCH=0).
        const float sdrWhite = resolveSdrWhiteNits();
        const float refWhite = envFloat("RV_HDR_PQ_REF_WHITE", 100.f);
        if (refWhite <= 0.f)
            return 1.f;
        const float scale = sdrWhite / refWhite;

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            cout << "INFO: linear present SDR-white match ×" << scale
                 << " (sdrWhite=" << sdrWhite << " nits, refWhite=" << refWhite
                 << "; RV_HDR_LINEAR_SDR_MATCH=0 to disable)" << endl;
        }
        return scale;
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
        m_container->setFocusPolicy(Qt::NoFocus);
        // createWindowContainer can reset window flags; re-assert input passthrough.
        m_window->setFlags(m_window->flags() | Qt::WindowTransparentForInput);
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

    void VulkanPresentWidget::setFrameHalf(int width, int height, std::vector<uint16_t> rgba16)
    {
        if (m_window)
            m_window->setFrameHalf(width, height, std::move(rgba16));
    }

    void VulkanPresentWidget::setHdrPresent(bool enabled)
    {
        if (m_window)
            m_window->setHdrPresent(enabled);
    }

    bool VulkanPresentWidget::hdrPresent() const { return m_window ? m_window->hdrPresent() : false; }

    bool VulkanPresentWidget::usingGpuInterop() const
    {
        return m_window && m_window->usingGpuInterop();
    }

    bool VulkanPresentWidget::ensureGpuInterop(QOpenGLContext* glctx, const QSize& pixelSize, bool float16)
    {
        return m_window && m_window->ensureGpuInterop(glctx, pixelSize, float16);
    }

    bool VulkanPresentWidget::blitFromGlFramebuffer(unsigned int srcFbo, int width, int height)
    {
        return m_window && m_window->blitFromGlFramebuffer(srcFbo, width, height);
    }

    void VulkanPresentWidget::presentGpuInteropFrame()
    {
        if (m_window)
            m_window->presentGpuInteropFrame();
    }

} // namespace Rv
