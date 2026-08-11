//******************************************************************************
// Copyright (c) 2007 Tweak Inc.
// All rights reserved.
//
// SPDX-License-Identifier: Apache-2.0
//
//******************************************************************************

#ifdef PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <RvCommon/GLView.h>
#include <RvCommon/QTGLVideoDevice.h>
#include <RvCommon/VulkanPresentWidget.h>
#include <TwkGLF/GLFence.h>
#include <RvCommon/InitGL.h>
#include <RvCommon/RvDocument.h>
#include <RvApp/Options.h>
#include <iostream>
#include <TwkApp/Event.h>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>

#include <QtWidgets/QMenu>
#include <QColorSpace>
#include <QGuiApplication>
#include <QWindow>
#include <QPalette>
#include <QPainter>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

namespace Rv
{
    using namespace boost;
    using namespace std;
    using namespace TwkApp;
    using namespace IPCore;

    namespace
    {
        // Goal 2: on-screen HDR / extended-range. Activated by RV_HDR=1 (or
        // true/yes/on). Prefer native Wayland so Qt can use
        // wp_color_management_v1; XWayland still clamps most clients as SDR.
        bool wantHdrDisplay()
        {
            const char* e = getenv("RV_HDR");
            if (!e || !*e)
                return false;
            if (!strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no"))
                return false;
            return true;
        }

        // Must stay in sync with DisplayIPNode::wantP3ExtendedEncoding(): that
        // decides what goes *into* the buffer, this decides how the surface is
        // tagged. See the comment there for the platform defaults.
        bool wantP3ExtendedEncoding()
        {
            const char* e = getenv("RV_HDR_ENCODING");
            if (e && *e)
            {
                if (!strcasecmp(e, "p3extended") || !strcasecmp(e, "p3"))
                    return true;
                if (!strcasecmp(e, "pq") || !strcasecmp(e, "st2084"))
                    return false;
            }
#ifdef PLATFORM_DARWIN
            return true;
#else
            return false;
#endif
        }

        bool envFlagOn(const char* name)
        {
            const char* e = getenv(name);
            if (!e || !*e)
                return false;
            if (!strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no"))
                return false;
            return true;
        }

        bool envFlagOff(const char* name)
        {
            const char* e = getenv(name);
            if (!e || !*e)
                return false;
            return !strcmp(e, "0") || !strcmp(e, "false") || !strcmp(e, "off") || !strcmp(e, "no");
        }

        // Qt OpenGL → Wayland present is broken on this NVIDIA/Hyprland stack.
        // Prefer Vulkan QRhi present (HDR-capable path). Fall back to CPU QWidget
        // if RV_WAYLAND_CPU_PRESENT=1 or RV_VULKAN_PRESENT=0.
        bool wantExternalPresent()
        {
            if (envFlagOff("RV_WAYLAND_PRESENT"))
                return false;
            if (envFlagOn("RV_WAYLAND_PRESENT"))
                return true;
            return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
        }

        bool preferVulkanPresent()
        {
            if (envFlagOff("RV_VULKAN_PRESENT"))
                return false;
            if (envFlagOn("RV_VULKAN_PRESENT"))
                return true;
            // Default on for Wayland; CPU only if explicitly forced.
            if (envFlagOn("RV_WAYLAND_CPU_PRESENT"))
                return false;
            return true;
        }

        // Paints a full-widget QImage (from grabFramebuffer). Mouse events pass
        // through so GLView keeps input. SDR-only emergency fallback.
        class PresentOverlay : public QWidget
        {
        public:
            explicit PresentOverlay(QWidget* parent)
                : QWidget(parent)
            {
                setAttribute(Qt::WA_TransparentForMouseEvents, true);
                setAttribute(Qt::WA_OpaquePaintEvent, true);
                setAttribute(Qt::WA_NoSystemBackground, true);
                setAutoFillBackground(false);
            }

            void setFrame(QImage img)
            {
                m_img = std::move(img);
                update();
            }

        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter p(this);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                if (m_img.isNull())
                {
                    p.fillRect(rect(), Qt::black);
                    return;
                }
                p.drawImage(rect(), m_img);
            }

        private:
            QImage m_img;
        };

        class SyncBufferThreadData
        {
        public:
            explicit SyncBufferThreadData(const VideoDevice* device)
                : m_device(device)
                , m_done(false)
                , m_doSync(false)
                , m_running(false)
                , m_mutex()
                , m_cond()
            {
            }

            void run()
            {
                while (!m_done)
                {
                    boost::mutex::scoped_lock lock(m_mutex);
                    m_running = true;
                    m_doSync = false;

                    m_cond.wait(lock);

                    if (m_device && m_doSync && !m_done)
                        m_device->syncBuffers();

                    m_doSync = false;
                }
            }

            void notify(bool finish = false)
            {
                {
                    boost::mutex::scoped_lock lock(m_mutex);
                    if (finish)
                        m_done = true;
                    else
                        m_doSync = true;
                    if (!m_running)
                        return;
                }

                m_cond.notify_one();
            }

            const VideoDevice* device() const { return m_device; }

            void setDevice(const VideoDevice* d) { m_device = d; }

        private:
            SyncBufferThreadData(SyncBufferThreadData&) {}

            void operator=(SyncBufferThreadData&) {}

        private:
            bool m_done;
            bool m_doSync;
            bool m_running;
            boost::mutex m_mutex;
            boost::condition_variable m_cond;
            const VideoDevice* m_device;
        };

        class ThreadTrampoline
        {
        public:
            ThreadTrampoline(GLView* view)
                : m_view(view)
            {
            }

            void operator()()
            {
                SyncBufferThreadData* closure = reinterpret_cast<SyncBufferThreadData*>(m_view->syncClosure());
                closure->run();
            }

        private:
            GLView* m_view;
        };

    } // namespace

    GLView::GLView(QWidget* parent, QOpenGLContext* sharedContext, RvDocument* doc, bool stereo, bool vsync, bool doubleBuffer, int red,
                   int green, int blue, int alpha, bool noResize)
        : QOpenGLWidget(parent)
        , m_sharedContext(sharedContext)
        , m_doc(doc)
        , m_red(red)
        , m_green(green)
        , m_blue(blue)
        , m_alpha(alpha)
        , m_lastKey(0)
        , m_lastKeyType(QEvent::None)
        , m_userActive(true)
        , m_renderCount(0)
        , m_firstPaintCompleted(false)
        , m_csize(1024, 576)
        , m_msize(128, 128)
        , m_postFirstNonEmptyRender(noResize)
        , m_stopProcessingEvents(false)
        , m_syncThreadData(0)
        , m_presentOverlay(nullptr)
    {
        setFormat(rvGLFormat(stereo, vsync, doubleBuffer, red, green, blue, alpha));

        // Wayland composites QOpenGLWidget FBOs with alpha. If RGB draws leave
        // alpha at 0, the image plane is fully transparent and the *desktop*
        // shows through — reads as an "empty" viewer. Mark the widget opaque.
        // Do NOT setPalette()/setAutoFillBackground() here: those emit events
        // during construction before m_videoDevice exists → QTTranslator SIGSEGV.
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);

        if (wantHdrDisplay())
        {
            const QSurfaceFormat requested = format();
            cout << "INFO: HDR display mode (RV_HDR): colorSpace tf="
                 << int(requested.colorSpace().transferFunction())
                 << " rgb bits=" << requested.redBufferSize() << "/" << requested.greenBufferSize()
                 << "/" << requested.blueBufferSize()
                 << " platform=" << QGuiApplication::platformName().toStdString() << endl;
        }

        ostringstream str;
        str << UI_APPLICATION_NAME " Main Window" << "/" << m_doc;
        m_videoDevice = new QTGLVideoDevice(0, str.str(), this);

        // External present is normally attached via setExternalPresentWidget()
        // after construction (Vulkan surface must be created *before* this
        // QOpenGLWidget so the top-level window uses Vulkan composition).
        // CPU fallback can still be created here if forced.
        if (wantExternalPresent() && !preferVulkanPresent())
        {
            m_presentOverlay = new PresentOverlay(this);
            cout << "INFO: Wayland CPU present fallback (SDR only). "
                    "Set RV_VULKAN_PRESENT=1 for Vulkan/HDR path."
                 << endl;
            syncPresentOverlayGeometry();
            m_presentOverlay->show();
            m_presentOverlay->raise();
        }

        setObjectName((m_doc->session()) ? m_doc->session()->name().c_str() : "no session");

        m_activityTimer.start();
        setMouseTracking(true);
        setAcceptDrops(true);
        setFocusPolicy(Qt::StrongFocus);

        m_eventProcessingTimer.setSingleShot(true);
        connect(&m_eventProcessingTimer, SIGNAL(timeout()), this, SLOT(eventProcessingTimeout()));
    }

    GLView::~GLView()
    {
        // delete m_frameBuffer;
        delete m_videoDevice;

        if (m_syncThreadData)
        {
            SyncBufferThreadData* closure = reinterpret_cast<SyncBufferThreadData*>(m_syncThreadData);
            closure->notify(true);
            m_swapThread.join();

            delete closure;
        }
    }

    void GLView::stopProcessingEvents() { m_stopProcessingEvents = true; }

    void GLView::setExternalPresentWidget(QWidget* present)
    {
        if (!present)
            return;
        m_presentOverlay = present;
        // Ensure the overlay never steals image-area input (pan, grade, scrub).
        m_presentOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_presentOverlay->setFocusPolicy(Qt::NoFocus);
        for (QWidget* w : m_presentOverlay->findChildren<QWidget*>())
        {
            w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            w->setFocusPolicy(Qt::NoFocus);
        }
        // Backup: if the native present surface still gets events, forward them.
        m_presentOverlay->installEventFilter(this);
        if (auto* ps = dynamic_cast<PresentSurface*>(present))
        {
            ps->setHdrPresent(wantHdrDisplay());
            cout << "INFO: external present surface attached (GL FBO → texture). HDR request="
                 << (wantHdrDisplay() ? "yes" : "no") << endl;
        }
        syncPresentOverlayGeometry();
        m_presentOverlay->show();
        m_presentOverlay->raise();
        // Keep keyboard/mouse focus on the GL view for RV tools.
        setFocus(Qt::OtherFocusReason);
    }

    QSize GLView::sizeHint() const { return m_csize; }

    QSize GLView::minimumSizeHint() const { return m_msize; }

    void GLView::absolutePosition(int& x, int& y) const
    {
        x = 0;
        y = 0;

        QPoint p(0, 0);
        QPoint gp = mapToGlobal(p);

        x = gp.x();
        y = gp.y();
    }

    QSurfaceFormat GLView::rvGLFormat(bool stereo, bool vsync, bool doubleBuffer, int red, int green, int blue, int alpha)
    {
        const Rv::Options& opts = Rv::Options::sharedOptions();
        const bool hdr = wantHdrDisplay();

        // NOTE_QT6: QGLFormat into QSurfaceFormat
        // NOTE_QT6: setStencil, setDepth does not exist anymore. Trying to use
        // setDepthBufferSize and setStencilBufferSize.
        QSurfaceFormat fmt;
        fmt.setDepthBufferSize(24);
        fmt.setSwapBehavior(doubleBuffer ? QSurfaceFormat::DoubleBuffer : QSurfaceFormat::SingleBuffer);
        fmt.setStencilBufferSize(8);
        fmt.setStereo(stereo);

        fmt.setRenderableType(QSurfaceFormat::OpenGL);

        // NOTE_QT: Set to version 2.1 for now.
        fmt.setMajorVersion(2);
        fmt.setMinorVersion(1);

        // fmt.setProfile(QSurfaceFormat::CoreProfile);
        // fmt.setProfile(QSurfaceFormat::CompatibilityProfile);

        //
        //  The default value for these buffer sizes is -1, but it is
        //  illegal to set to that value so test for positive red, not
        //  just non-zero red.  If any of these values is < 0, we ignore it.
        //
        // HDR: do NOT tag the QOpenGLWidget surface as Bt2100Pq when we blit
        // via the external Vulkan present path. The GL FBO is only a transfer
        // buffer of raw PQ codes (UNORM); HDR10 color management belongs on the
        // Vulkan swapchain. Tagging the GL widget confuses Qt/EGL and can make
        // the image look like muted SDR while HUD greens look "HDR".
        //
        // Optional legacy: RV_HDR_GL_SURFACE=1 still requests Bt2100Pq on GL.
        // Do not force 10/16 bpc: NVIDIA Wayland EGL often has no matching
        // config (QEGLPlatformContext 3009).
        if (hdr)
        {
            const char* glSurf = getenv("RV_HDR_GL_SURFACE");
            const bool tagGl = glSurf && *glSurf && strcmp(glSurf, "0") && strcmp(glSurf, "false")
                               && strcmp(glSurf, "off") && strcmp(glSurf, "no");
            // Only tag GL when not using Vulkan present (CPU/off paths).
            const bool vulkanPresent = preferVulkanPresent() && wantExternalPresent();
            if (tagGl || !vulkanPresent)
            {
                // Must match what DisplayIPNode encodes into the buffer, or the
                // present shader decodes with the wrong transfer.
                fmt.setColorSpace(wantP3ExtendedEncoding()
                                      ? QColorSpace(QColorSpace::DisplayP3)
                                      : QColorSpace(QColorSpace::Bt2100Pq));
            }
        }

        if (red > 0)
            fmt.setRedBufferSize(red);
        if (green > 0)
            fmt.setGreenBufferSize(green);
        if (blue > 0)
            fmt.setBlueBufferSize(blue);
        if (alpha >= 0)
        {
            fmt.setAlphaBufferSize(alpha);
        }

        // Wayland composites QOpenGLWidget FBOs with alpha. We need an alpha
        // *plane* so paintGL can force A=1 after render; alphaBufferSize=0
        // leaves Qt/RHI with a translucent texture (desktop shows through).
        // Prefer 8-bit alpha on Wayland even if prefs say 0.
        if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        {
            if (fmt.alphaBufferSize() <= 0)
                fmt.setAlphaBufferSize(8);
        }
        else if (alpha == 0)
        {
            fmt.setAlphaBufferSize(0);
        }

        fmt.setSwapInterval(vsync ? 1 : 0);

        return fmt;
    }

    void GLView::initializeGL()
    {
        //
        //  At this point the format is known. Can't do this in the constructor
        //

        // QUESTION_QT6: Should we use isValid from QOpenGLWidget or directly
        // using from QOpenGLContext? NOTE_QT6: Returns true if the widget and
        // OpenGL resources, like the context, have been successfully
        // initialized.
        //           Note that the return value is always false until the widget
        //           is shown.
        // NOTE_QT6: QOpenGLContext: Returns if this context is valid, i.e. has
        // been successfully created.
        if (context()->isValid())
        {
            initializeGLExtensions();
            initializeOpenGLFunctions();

            // Keep PQ codes linear in the UNORM FBO (no sRGB encode on write).
#ifdef GL_FRAMEBUFFER_SRGB
            glDisable(GL_FRAMEBUFFER_SRGB);
#endif

            if (m_sharedContext)
            {
                context()->setShareContext(m_sharedContext);
            }

            if (m_doc)
            {
                m_doc->initializeSession();
            }

            // NOTE_QT6: QGLFormat is deprecated. Using QSurfaceFormat now.
            QSurfaceFormat f = context()->format();
            // Always log alpha on Wayland — A=0 FBO + compositor = desktop bleed.
            if (wantHdrDisplay()
                || QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            {
                cout << "INFO: GL context format: rgb bits=" << f.redBufferSize() << "/"
                     << f.greenBufferSize() << "/" << f.blueBufferSize()
                     << " alpha=" << f.alphaBufferSize()
                     << " colorSpace primaries=" << int(f.colorSpace().primaries())
                     << " tf=" << int(f.colorSpace().transferFunction())
                     << " platform=" << QGuiApplication::platformName().toStdString() << endl;
            }

#ifndef PLATFORM_DARWIN
            //
            //  Doesn't work on OS X
            //
            if (f.redBufferSize() != m_red && m_red != 0)
            {
                // QMessageBox box(this);
                // box.setWindowTitle(tr("Ouput Display Format"));

                ostringstream str;

                str << "WARNING: asked for"
                    << " " << m_red << " " << m_green << " " << m_blue << " " << m_alpha << " RGBA color but got"
                    << " " << f.redBufferSize() << " " << f.greenBufferSize() << " " << f.blueBufferSize() << " "
                    << (f.alphaBufferSize() <= 0 ? 0 : f.alphaBufferSize()) << " RGBA instead";

                cout << str.str() << endl;

                // box.setText(str.str().c_str());
                // box.setDetailedText("You can change the default display color
                // depth and target "
                //                     "from the preferences under
                //                     Rendering->Display Output Format.\n"
                //                     "Choosing \"OpenGL Default Format\" will
                //                     tell RV to ask for the " "default
                //                     prefered format for this display.");
                // box.setWindowModality(Qt::WindowModal);
                // QPushButton* b1 = box.addButton(tr("Continue"),
                // QMessageBox::AcceptRole); box.setIcon(QMessageBox::Critical);
                // box.exec();
            }
#endif
            if (f.stencilBufferSize() == 0)
            {
                cout << "WARNING: no stencil buffer available" << endl;
            }
        }
        else
        {
            cout << "WARNING: invalid GL context" << endl;
        }
    }

    void GLView::resizeGL(int w, int h)
    {
        if (m_doc)
            m_doc->viewSizeChanged(w, h);
#ifdef PLATFORM_WINDOWS
        SetWindowRgn(reinterpret_cast<HWND>(this->winId()), 0, false);
#endif
    }

    bool GLView::validateReadPixels(int x, int y, int w, int h)
    {
        int r = x + w;
        int t = y + h;

        // are the extents of the read region out of bounds?
        if (x < 0 || y < 0 || r > width() * devicePixelRatio() || t > height() * devicePixelRatio())
            return false;

        return true;
    }

    QImage GLView::readPixels(int x, int y, int w, int h)
    {
        // If out of bounds, return an empty image.
        if (validateReadPixels(x, y, w, h) == false)
        {
            QImage image(0, 0, QImage::Format_RGBA8888);
            return image;
        }

        makeCurrent();

        QImage image(w, h, QImage::Format_RGBA8888);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());

        return image;
    }

    void GLView::debugSaveFramebuffer()
    {
        // Create a QImage with the same size as the FBO
        QImage image(width(), height(), QImage::Format_RGBA8888);
        glReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());

        // image.save("/home/<username>>/<orv_folder>/fbo.png");
    }

    void GLView::resizeEvent(QResizeEvent* event)
    {
        QOpenGLWidget::resizeEvent(event);
        syncPresentOverlayGeometry();
    }

    void GLView::moveEvent(QMoveEvent* event)
    {
        QOpenGLWidget::moveEvent(event);
        syncPresentOverlayGeometry();
    }

    void GLView::syncPresentOverlayGeometry()
    {
        if (!m_presentOverlay)
            return;

        // Stacked sibling (Vulkan container) or child (CPU overlay): fill our
        // area in the shared parent, or our own rect if we own it.
        if (m_presentOverlay->parentWidget() == this)
        {
            m_presentOverlay->setGeometry(rect());
        }
        else if (m_presentOverlay->parentWidget() == parentWidget())
        {
            m_presentOverlay->setGeometry(QRect(pos(), size()));
        }
        else if (m_presentOverlay->parentWidget())
        {
            const QPoint topLeft = mapTo(m_presentOverlay->parentWidget(), QPoint(0, 0));
            m_presentOverlay->setGeometry(QRect(topLeft, size()));
        }
        m_presentOverlay->raise();
        if (!m_presentOverlay->isVisible())
            m_presentOverlay->show();
    }

    PresentSurface* GLView::presentSurface() const
    {
        // PresentSurface is not a QObject, so this is dynamic_cast rather than
        // qobject_cast. Returns null when there is no overlay, or when the
        // overlay is some other widget.
        return dynamic_cast<PresentSurface*>(m_presentOverlay);
    }

    bool GLView::needsFloatPresentTransfer() const
    {
        PresentSurface* ps = presentSurface();
        return ps && ps->needsFloatTransfer();
    }

    GLuint GLView::presentFramebufferObject() const
    {
        if (m_floatPresentFbo && needsFloatPresentTransfer())
            return m_floatPresentFbo->handle();
        return defaultFramebufferObject();
    }

    void GLView::ensureFloatPresentFbo(const QSize& pixelSize)
    {
        if (!needsFloatPresentTransfer() || !pixelSize.isValid())
        {
            m_floatPresentFbo.reset();
            return;
        }
        if (m_floatPresentFbo && m_floatPresentFbo->size() == pixelSize)
            return;

        QOpenGLFramebufferObjectFormat fmt;
        fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        // 16F is enough for EDR headroom; render still uses scene/display linear
        // or gamma-encoded P3 Extended values that may exceed 1.0.
        fmt.setInternalTextureFormat(GL_RGBA16F);
        fmt.setSamples(0);
        m_floatPresentFbo = std::make_unique<QOpenGLFramebufferObject>(pixelSize, fmt);
        if (!m_floatPresentFbo->isValid())
        {
            cerr << "ERROR: float present FBO (RGBA16F) failed; falling back to 8-bit grab" << endl;
            m_floatPresentFbo.reset();
            return;
        }
        static bool once = false;
        if (!once)
        {
            once = true;
            cout << "INFO: GL float present FBO RGBA16F " << pixelSize.width() << "x" << pixelSize.height()
                 << " (p3extended EDR transfer)" << endl;
        }
    }

    void GLView::updateCpuPresentFallback()
    {
        presentExternalFrame();
    }

    void GLView::presentExternalFrame()
    {
        if (!m_presentOverlay)
            return;

        static bool s_inPresent = false;
        if (s_inPresent)
            return;
        s_inPresent = true;

        syncPresentOverlayGeometry();
        if (!m_presentOverlay->isVisible())
            m_presentOverlay->show();

        // Backend-neutral: Vulkan on Linux, Metal on macOS. Null if the overlay
        // is not a present surface at all.
        auto* vk = presentSurface();

        // Prefer GPU interop: blit FBO → shared image (no readback). Backends
        // without interop return false from ensureGpuInterop and fall through
        // to the CPU transfer below.
        if (vk && QOpenGLContext::currentContext())
        {
            const bool wantFloat = needsFloatPresentTransfer();
            GLuint srcFbo = presentFramebufferObject();
            if (srcFbo == 0)
                srcFbo = defaultFramebufferObject();

            // Use the *actual* bound present FBO size (viewport set in paintGL),
            // not a re-derived width*dpr which can be off-by-one vs the widget FBO.
            GLint vp[4] = {0, 0, 0, 0};
            GLint prevFbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
            glGetIntegerv(GL_VIEWPORT, vp);
            glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
            int fw = vp[2];
            int fh = vp[3];
            if (fw <= 0 || fh <= 0)
            {
                fw = std::max(1, int(std::lround(width() * devicePixelRatioF())));
                fh = std::max(1, int(std::lround(height() * devicePixelRatioF())));
            }
            const QSize px(fw, fh);

            if (vk->ensureGpuInterop(QOpenGLContext::currentContext(), px, wantFloat))
            {
                if (vk->blitFromGlFramebuffer(srcFbo, px.width(), px.height()))
                {
                    glFlush();
                    glFinish(); // serialize GL write before Vulkan copy/sample
                    vk->presentGpuInteropFrame();
                    s_inPresent = false;
                    return;
                }
            }
        }

        // CPU readback fallback (slower; used if interop unavailable).
        // GL FBO is RGBA16F — read half floats (not float32) so upload matches RGBA16F
        // and we move half the bytes over PCIe vs GL_FLOAT.
        if (vk && m_floatPresentFbo && m_floatPresentFbo->isValid() && needsFloatPresentTransfer())
        {
            const int w = m_floatPresentFbo->width();
            const int h = m_floatPresentFbo->height();
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
            std::vector<uint16_t> pixels(size_t(w) * size_t(h) * 4);
            m_floatPresentFbo->bind();
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_HALF_FLOAT, pixels.data());
            const GLenum readErr = glGetError();
            m_floatPresentFbo->release();

            if (readErr != GL_NO_ERROR)
            {
                static bool once = false;
                if (!once)
                {
                    once = true;
                    cerr << "ERROR: glReadPixels HALF_FLOAT failed 0x" << hex << readErr << dec
                         << "; falling back to 8-bit grab" << endl;
                }
                // Fall through to grabFramebuffer below.
            }
            else
            {
                // Context object must be a QObject; PresentSurface is not one,
                // so use the overlay widget for lifetime tracking.
                QTimer::singleShot(0, m_presentOverlay,
                                   [vk, w, h, data = std::move(pixels)]() mutable {
                                       vk->setFrameHalf(w, h, std::move(data));
                                   });
                s_inPresent = false;
                return;
            }
        }

        QImage img = grabFramebuffer();
        if (img.isNull())
        {
            s_inPresent = false;
            return;
        }
        img.setDevicePixelRatio(1.0);

        if (vk)
        {
            QTimer::singleShot(0, m_presentOverlay, [vk, img = std::move(img)]() mutable {
                vk->setFrame(std::move(img));
            });
        }
        else if (auto* overlay = dynamic_cast<PresentOverlay*>(m_presentOverlay))
        {
            QTimer::singleShot(0, overlay, [overlay, img = std::move(img)]() mutable {
                overlay->setFrame(std::move(img));
            });
        }

        s_inPresent = false;
    }

    void GLView::paintGL()
    {
        TWK_GLDEBUG;

        IPCore::Session* session = m_doc->session();
        bool debug = IPCore::debugProfile && session;

        if (!m_postFirstNonEmptyRender && session && session->postFirstNonEmptyRender())
        {
            m_postFirstNonEmptyRender = true;

            if (!session->isFullScreen())
            {
                m_doc->resizeToFit(false, false);
                m_doc->center();
                TWK_GLDEBUG;
            }
        }

        if (debug)
        {
            Session::ProfilingRecord& trecord = session->beginProfilingSample();
            trecord.renderStart = session->profilingElapsedTime();
        }

        // should be no longer necessary, moved it to resize()
#if 0
    //
    //  This is necessary to stop the Windows Desktop Window Manager
    //  (new in Vista/win7) fromc caching portions of rv's glview
    //  and holding them in the display even when rv redraws.  the
    //  effect being that parts of previous displays will be "left
    //  behind" and not updated even when rv plays.  especially when
    //  going to/from fullscreen.
    //
#ifdef PLATFORM_WINDOWS
    SetWindowRgn (this->winId(), 0, false);
#endif
#endif

        // Bind present target: float FBO for p3extended (EDR >1), else widget FBO.
        // On Wayland a zero alpha makes the plane show the desktop after composite.
        const QSize pixelSize(std::max(1, int(std::lround(width() * devicePixelRatioF()))),
                              std::max(1, int(std::lround(height() * devicePixelRatioF()))));
        ensureFloatPresentFbo(pixelSize);

        GLuint targetFbo = presentFramebufferObject();
        if (targetFbo == 0 && QOpenGLContext::currentContext())
            targetFbo = QOpenGLContext::currentContext()->defaultFramebufferObject();
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, targetFbo);
        TWK_GLDEBUG;
        glViewport(0, 0, pixelSize.width(), pixelSize.height());
        TWK_GLDEBUG;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        TWK_GLDEBUG;
        glClearColor(0.f, 0.f, 0.f, 1.0f);
        TWK_GLDEBUG;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        TWK_GLDEBUG;

        if (m_doc && session && m_videoDevice)
        {
            // m_frameBuffer->makeCurrent();
            TWK_GLDEBUG;
            m_videoDevice->makeCurrent();
            TWK_GLDEBUG;

            if (m_userActive && m_activityTimer.elapsed() > 1.0)
            {
                if (m_doc->mainPopup() && !m_doc->mainPopup()->isVisible() && hasFocus())
                {
                    TwkApp::ActivityChangeEvent aevent("user-inactive", m_videoDevice);
                    m_videoDevice->sendEvent(aevent);
                    TWK_GLDEBUG;
                    m_userActive = false;
                }
            }

            //
            //  Make sure the video device knows where it is on screen.
            //
            int x = 0, y = 0;
            absolutePosition(x, y);
            m_videoDevice->setAbsolutePosition(x, y);

            TWK_GLDEBUG;
#ifdef GL_FRAMEBUFFER_SRGB
            // Re-assert each frame: some paths re-enable sRGB.
            glDisable(GL_FRAMEBUFFER_SRGB);
#endif
            session->render();
            TWK_GLDEBUG;

            m_firstPaintCompleted = true;

            // Force alpha = 1 on the *present* FBO (float FBO for p3extended,
            // else the widget default FBO). Binding only the default FBO left
            // the float present target without the opaque alpha fix.
            GLuint postFbo = presentFramebufferObject();
            if (postFbo == 0)
                postFbo = QOpenGLContext::currentContext()->defaultFramebufferObject();
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, postFbo);
            TWK_GLDEBUG;

            // FBO probe (RV_GL_PROBE=1): log first 5 paints + every 30th after.
            // Also samples L/C/R of the image (for PQ: ~0.51/0.75/0.64 @ 100/1000/400 nits).
            if (getenv("RV_GL_PROBE") && *getenv("RV_GL_PROBE") != '0')
            {
                static int probeCount = 0;
                if (probeCount < 5 || (probeCount % 30) == 0)
                {
                    GLint vp[4] = {0, 0, 0, 0};
                    glGetIntegerv(GL_VIEWPORT, vp);
                    auto sample = [&](float fx, float fy) {
                        unsigned char rgba[4] = {0, 0, 0, 0};
                        const int px = vp[0] + int(std::max(0.f, std::min(float(vp[2] - 1), fx * vp[2])));
                        const int py = vp[1] + int(std::max(0.f, std::min(float(vp[3] - 1), fy * vp[3])));
                        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                        return rgba[0]; // grayscale wedge
                    };
                    // Image roughly in vertical center; L/C/R horizontally.
                    const int L = sample(0.20f, 0.50f);
                    const int C = sample(0.50f, 0.50f);
                    const int R = sample(0.80f, 0.50f);
                    cout << "INFO: RV_GL_PROBE n=" << probeCount << " fbo=" << postFbo
                         << " vp=" << vp[2] << "x" << vp[3]
                         << " LCR8=" << L << "," << C << "," << R
                         << " (PQ100≈130 PQ400≈164 PQ1000≈192 if HDR encode)"
                         << " dpr=" << devicePixelRatio()
                         << " widget=" << width() << "x" << height() << endl;
                }
                ++probeCount;
            }

            glPushAttrib(GL_COLOR_BUFFER_BIT);
            TWK_GLDEBUG;
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            TWK_GLDEBUG;
            glClearColor(0.f, 0.f, 0.f, 1.0f);
            TWK_GLDEBUG;
            glClear(GL_COLOR_BUFFER_BIT);
            TWK_GLDEBUG;
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            TWK_GLDEBUG;
            glPopAttrib();
            TWK_GLDEBUG;

            // Wayland: Qt may never composite this FBO into the window. Blit to
            // a QWidget child that *does* present (verified with plain QWidget).
            updateCpuPresentFallback();
        }

        if (m_stopProcessingEvents)
            return;

        //
        //  We're done with drawing and about to (possibly) wait for
        //  vsync before the buffer swaps, but if the driver is layzy,
        //  it may not performs some requested gl actions until after
        //  the wait on vsync, in which case we get tearing.
        //
        //  A glFlush here should make these gl actions happen during
        //  the wait.
        //
        //  This could be a problem if further drawing is done to this
        //  buffer by Qt, for example if it's doing it's graphics area
        //  widget drawing.  Seems fine for now.
        //
        //  Update: We want to actually wait until the gfx operations are
        //  complete.  glFlush just flushes the command buffer to
        //  hardware, glFinish blocks until the hardware is finished
        //  processing the commands.
        //

        // DONT
        // glFlush();
        // glFinish();

        //
        //  Do the swap, the vsync code is in the swapBuffers() code in
        //  Qt. It should block here waiting for the refresh. The debug
        //  code here is accumulating profiling information in the session
        //  ProfilingRecord struct. This can be dumped on exit to figure out
        //  what's going on.
        //

        // Note for Qt6 . QOpenGLWidget implementation:
        //
        // With the new QOpenGLWidget (Qt6 branch), all of the rendering of
        // each widget is done in its own FBOs (aka: off-screen buffers), as
        // opposed to the old Qt5/QGLWidget branch where the rendering was
        // done in each GGLWidget's backbuffer (aka: GL_BACK, an on-screen
        // framebuffer surface).
        //
        // With QOpenGLWidget, it is now the WainWindow's responsibility
        // (more technically, the MainWindow's rendering backend, which
        // happens to be Qt's OpenGL rendering backend when QOpenGLWidget are
        // present in the children tree) to gather and composite all of the
        // off-screen buffers (regardless of which image type they are, eg:
        // cpu-memory images, gpu/opengl images, etc) and finally to call
        // swapBuffers to show the final contents of the mainWindow.
        //
        // As a result, I'm not sure there's a point -- at all --
        // in calling swapBuffers on the GLView anywhere amnymore. From old
        // comments in the previous version of this tile, it appears that
        // calling swapBuffers was done to force a quicker visual update, or
        // to minimize visual tearing of some sort. This would have worked
        // with the old QGLWidget (becayuse each QGLWidget had its own
        // context, and its rendering target was directly the GL_BACK
        // framebuffer, but, again, with QOpenGLWidget, the rendering target
        // is no longer GL_BACK, it is an FBO that is meant to be used at
        // the end of the application's drawing / visual update pipeline.

        if (debug)
        {
            Session::ProfilingRecord& trecord = session->currentProfilingSample();
            trecord.renderEnd = session->profilingElapsedTime();
            trecord.swapStart = trecord.renderEnd;

            if (session->outputVideoDevice() != videoDevice())
            {
                session->outputVideoDevice()->syncBuffers();
            }
            else
            {
                m_videoDevice->widget()->context()->swapBuffers(m_videoDevice->widget()->context()->surface());
            }

            trecord.swapEnd = session->profilingElapsedTime();
            session->endProfilingSample();
        }
        else
        {
            if (session->outputVideoDevice() != videoDevice())
            {
                session->outputVideoDevice()->syncBuffers();
            }
        }

        session->addSyncSample();

        session->postRender();

        m_eventProcessingTimer.start();

        TWK_GLDEBUG;
    }

    void GLView::eventProcessingTimeout() { m_doc->session()->userGenericEvent("per-render-event-processing", ""); }

    bool GLView::event(QEvent* event)
    {
        // qDebug() << "Event type: " << event->type();

        bool keyevent = false;
        Rv::Session* session = m_doc->session();

        if (m_stopProcessingEvents)
        {
            event->accept();
            return true;
        }

        //
        //  We want to exclude the "click-through" clicks on
        //  click-to-focus systems (like osX).  Turns out the event
        //  sequence in these cases is exactly the same as a
        //  tab-to-focus, then click sequence.  So the only way we have
        //  to identify the "click-through" is by how quickly the click
        //  (button push) follows the windowActivate event.
        //

        if (event->type() == QEvent::WindowActivate)
            m_activationTimer.start();

#if 0
    //
    //  This doesn't work.  Nothing I've tried will make a stylus
    //  press raise and activate the window when we are handling
    //  native tablet events.  It works fine when we are treating
    //  stylus events as mouse events.
    //
    if (event->type() == QEvent::TabletPress && !isActiveWindow())
    {
        cerr << "activating window" << endl;
        QEvent activateEvent(QEvent::WindowActivate);
        //m_frameBuffer->translator().sendQTEvent(new QEvent(QEvent::WindowActivate));
        session->setEventVideoDevice(videoDevice());
        m_videoDevice->translator().sendQTEvent(new QEvent(QEvent::WindowActivate));
        event->accept();
        /*
        activateWindow();
        raise();
        event->accept();
        */
        return true;
    }
#endif

        float activationTime = 0.0;
        if (m_activationTimer.isRunning())
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                //
                //  Pass this time through with the event, so that
                //  event handling code can decide witehr or not to ignore
                //  this 'click-through' event.
                //
                activationTime = m_activationTimer.elapsed();
                m_activationTimer.stop();
            }
            if (event->type() == QEvent::MouseMove)
                m_activationTimer.stop();
        }

        if (event->type() != QEvent::Paint)
        {
            m_activityTimer.stop();
            m_activityTimer.start();

            if (!m_userActive)
            {
                TwkApp::ActivityChangeEvent aevent("user-active", m_videoDevice);

                //
                //  m_userActive set first will prevent recursive nightmare. In
                //  Qt 4.4 an event may recursively produce more
                //  events. For example, changing the cursor in 4.4 will
                //  cause a Paint event immediately (not after the
                //  previous event returns).
                //

                m_userActive = true;
                m_videoDevice->sendEvent(aevent);
            }
        }

        if (QKeyEvent* kevent = dynamic_cast<QKeyEvent*>(event))
        {
            keyevent = true;

            //
            //  Get around really annoying event bugs on Qt/Mac
            //

            if (m_lastKey == kevent->key()
                && (m_lastKeyType == QEvent::ShortcutOverride && (kevent->type() == QEvent::KeyPress) || (m_lastKeyType == kevent->type())))
            {
                //
                //  Qt 4.3.3 (4.5 too) will give both override and press events
                //  even if key is not in menu. Filter that here.
                //
                //  Remember the new key/type, otherwise we filter out
                //  any number of ShortcutOverride/Press pairs, and
                //  auto-repeat doesn't work.
                //
                m_lastKey = kevent->key();
                m_lastKeyType = kevent->type();

                event->accept();
                return true;
            }

            m_lastKeyType = kevent->type();
            m_lastKey = kevent->key();
        }

        switch (event->type())
        {
        case QEvent::FocusIn:
            m_videoDevice->translator().resetModifiers();
        case QEvent::Enter:
            setFocus(Qt::MouseFocusReason);
            break;
        default:
            break;
        }
        if (event->type() == QEvent::Resize)
        {
            QResizeEvent* e = static_cast<QResizeEvent*>(event);

            // QT5 BUG -- results in invalid drawable
            if (!isVisible())
                return true;

            if (e->oldSize().width() != -1 && e->oldSize().height() != -1)
            {
                ostringstream contents;
                contents << e->oldSize().width() << " " << e->oldSize().height() << "|" << e->size().width() << " " << e->size().height();

                if (m_doc && session)
                {
                    session->userGenericEvent("view-resized", contents.str());
                }
            }
            return QOpenGLWidget::event(event);
        }

        if (session && session->outputVideoDevice()
            && session->outputVideoDevice()->displayMode() == TwkApp::VideoDevice::MirrorDisplayMode)
        {
            if (const TwkApp::VideoDevice* cdv = session->controlVideoDevice())
            {
                const TwkApp::VideoDevice* odv = session->outputVideoDevice();

                if (odv && cdv != odv && cdv == videoDevice())
                {
                    const float w = width();
                    const float h = height();
                    const float ow = odv->width();
                    const float oh = odv->height();

                    const float aspect = w / h;
                    const float oaspect = ow / oh;

                    m_videoDevice->translator().setRelativeDomain(ow, oh);

                    if (aspect >= oaspect)
                    {
                        const float yscale = oh / h;
                        const float yoffset = 0.0;
                        const float xscale = yscale;
                        const float xoffset = -(w * yscale - ow) / 2.0;
                        m_videoDevice->translator().setScaleAndOffset(xoffset, yoffset, xscale, yscale);
                    }
                    else
                    {
                        const float xscale = ow / w;
                        const float xoffset = 0.0;
                        const float yscale = xscale;
                        const float yoffset = -(xscale * h - oh) / 2.0;
                        m_videoDevice->translator().setScaleAndOffset(xoffset, yoffset, xscale, yscale);
                    }
                }
                else
                {
                    m_videoDevice->translator().setScaleAndOffset(0, 0, 1.0, 1.0);
                    m_videoDevice->translator().setRelativeDomain(width(), height());
                }
            }
            else
            {
                m_videoDevice->translator().setScaleAndOffset(0, 0, 1.0, 1.0);
                m_videoDevice->translator().setRelativeDomain(width(), height());
            }
        }
        else
        {
            m_videoDevice->translator().setScaleAndOffset(0, 0, 1.0, 1.0);
            m_videoDevice->translator().setRelativeDomain(width(), height());
        }

        if (session)
            session->setEventVideoDevice(videoDevice());

        if (m_videoDevice->translator().sendQTEvent(event, activationTime))
        {
            event->accept();
            return true;
        }
        else
        {
            bool result = QOpenGLWidget::event(event);

            return result;
        }
    }

    bool GLView::eventFilter(QObject* object, QEvent* event)
    {
        // Forward image-area input that landed on the present overlay/native
        // container so pan, E+drag, playhead, etc. still hit GLView/translator.
        if (m_presentOverlay
            && (object == m_presentOverlay || m_presentOverlay->isAncestorOf(qobject_cast<QWidget*>(object))))
        {
            switch (event->type())
            {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::Wheel:
            case QEvent::HoverMove:
            case QEvent::HoverEnter:
            case QEvent::HoverLeave:
            case QEvent::TabletPress:
            case QEvent::TabletRelease:
            case QEvent::TabletMove:
            case QEvent::NativeGesture:
            case QEvent::TouchBegin:
            case QEvent::TouchUpdate:
            case QEvent::TouchEnd:
            {
                // Map to GLView local coords and redeliver.
                if (auto* me = dynamic_cast<QMouseEvent*>(event))
                {
                    const QPointF local = mapFromGlobal(me->globalPosition());
                    QMouseEvent copy(me->type(), local, me->globalPosition(), me->scenePosition(), me->button(),
                                     me->buttons(), me->modifiers(), me->source());
                    QCoreApplication::sendEvent(this, &copy);
                    return true;
                }
                if (auto* we = dynamic_cast<QWheelEvent*>(event))
                {
                    const QPointF local = mapFromGlobal(we->globalPosition());
                    QWheelEvent copy(local, we->globalPosition(), we->pixelDelta(), we->angleDelta(), we->buttons(),
                                     we->modifiers(), we->phase(), we->inverted(), we->source());
                    QCoreApplication::sendEvent(this, &copy);
                    return true;
                }
                // Other pointer-like events: try as-is on this view.
                QCoreApplication::sendEvent(this, event);
                return true;
            }
            default:
                break;
            }
        }

        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease || event->type() == QEvent::Shortcut
            || event->type() == QEvent::ShortcutOverride)
        {

            //
            //  Get around really annoying event bugs on Qt/Mac
            //  (copied from GLView::event... same bug applies -lo)
            //
            //  Qt 4.3.3 (4.5 too) will give both override and press events even
            //  if key is not in menu. Filter that here.
            //
            //  Remember the new key/type, otherwise we filter out
            //  any number of ShortcutOverride/Press pairs, and
            //  auto-repeat doesn't work.
            //

            if (QKeyEvent* kevent = dynamic_cast<QKeyEvent*>(event))
            {
                if (m_lastKey == kevent->key()
                    && (m_lastKeyType == QEvent::ShortcutOverride && (kevent->type() == QEvent::KeyPress)
                        || (m_lastKeyType == kevent->type())))
                {
                    m_lastKey = kevent->key();
                    m_lastKeyType = kevent->type();

                    event->accept();
                    return true;
                }

                m_lastKeyType = kevent->type();
                m_lastKey = kevent->key();
            }

            // if (m_frameBuffer->translator().sendQTEvent(event))
            Session* session = m_doc->session();
            session->setEventVideoDevice(videoDevice());
            if (m_videoDevice->translator().sendQTEvent(event))
            {

                event->accept();
                return true;
            }

            event->accept();
            return true;
        }

        return false;
    }

    float GLView::devicePixelRatio() const
    {
        // Prefer the real QWidget/OpenGL FBO scale (fractional-scale safe).
        const qreal widgetDpr = QWidget::devicePixelRatioF();
        if (widgetDpr > 0.0)
            return float(widgetDpr);
        return videoDevice() ? videoDevice()->devicePixelRatio() : 1.0f;
    }

} // namespace Rv
