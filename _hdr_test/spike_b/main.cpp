//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Spike B — macOS EDR present-surface embedding.
//
// Question: can a Metal/QRhi EDR present surface be embedded over a live
// QOpenGLWidget using createWindowContainer inside a QStackedLayout(StackAll)?
// That is exactly how RvDocument stacks its present overlay over GLView
// (RvDocument.cpp ~222), so if the pattern works here it works there.
//
// What to look for:
//   1. Console reports EDR headroom > 1.0 on an XDR display.
//   2. The wedge steps above 1.0 get visibly brighter, rather than all
//      clipping to the same white. Screenshots tone-map — you must look at
//      the panel.
//   3. The Metal surface stays correctly stacked over the GL widget through
//      resize, fullscreen, and dragging between displays.
//
// Keys:  H toggle overlay   F fullscreen   I reprint EDR info   Q quit
//******************************************************************************

#include "edrinfo.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <rhi/qrhi.h>

#include <memory>

static QShader loadShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        qWarning() << "spike_b: cannot open shader" << path;
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

//------------------------------------------------------------------------------
// The SDR reference underneath: a plain QOpenGLWidget. Its only job is to be a
// live GL surface that the Metal window has to stack over, and to give an SDR
// white reference to compare the wedge against.
//------------------------------------------------------------------------------
class GlUnderlay : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit GlUnderlay(QWidget* parent = nullptr)
        : QOpenGLWidget(parent)
    {
    }

protected:
    void initializeGL() override { initializeOpenGLFunctions(); }

    void paintGL() override
    {
        // Mid grey, so it is obvious if the Metal overlay fails to cover it.
        glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
};

//------------------------------------------------------------------------------
// Metal/QRhi present surface. A QWindow (not a widget) so it owns its own
// graphics API, embedded later via createWindowContainer.
//------------------------------------------------------------------------------
class MetalPresentWindow : public QWindow
{
public:
    MetalPresentWindow()
    {
        setSurfaceType(QSurface::MetalSurface);
    }

    ~MetalPresentWindow() override { releaseRhi(); }

    EdrInfo lastEdr() const { return m_edr; }
    bool hdrSurfaceActive() const { return m_hdrSurface; }
    QString formatName() const { return m_formatName; }

protected:
    void exposeEvent(QExposeEvent*) override
    {
        if (isExposed())
        {
            initRhi();
            render();
        }
    }

    void resizeEvent(QResizeEvent*) override
    {
        if (isExposed() && m_sc)
            render();
    }

    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::UpdateRequest)
            render();
        else if (e->type() == QEvent::PlatformSurface
                 && static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
                        == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            releaseRhi();
        return QWindow::event(e);
    }

private:
    void initRhi()
    {
        if (m_rhi)
            return;

        QRhiMetalInitParams params;
        m_rhi.reset(QRhi::create(QRhi::Metal, &params));
        if (!m_rhi)
        {
            qCritical() << "spike_b: QRhi::create(Metal) FAILED";
            return;
        }
        qInfo() << "spike_b: QRhi backend =" << m_rhi->backendName();

        m_sc.reset(m_rhi->newSwapChain());
        m_sc->setWindow(this);

        // The EDR-capable format. Qt maps this to MTLPixelFormatRGBA16Float +
        // kCGColorSpaceExtendedLinearDisplayP3 + wantsExtendedDynamicRangeContent
        // (qrhimetal.mm chooseFormats/createOrResize).
        const QRhiSwapChain::Format wanted = QRhiSwapChain::HDRExtendedDisplayP3Linear;

        if (m_sc->isFormatSupported(wanted))
        {
            m_sc->setFormat(wanted);
            m_hdrSurface = true;
            m_formatName = "HDRExtendedDisplayP3Linear";
        }
        else
        {
            qWarning() << "spike_b: HDRExtendedDisplayP3Linear NOT supported; "
                          "falling back to SDR. EDR result will be negative.";
            m_formatName = "SDR";
        }
        qInfo() << "spike_b: swapchain format =" << m_formatName;

        m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
        m_sc->setRenderPassDescriptor(m_rp.get());
        m_sc->createOrResize();

        buildPipeline();
    }

    void buildPipeline()
    {
        static const float verts[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};

        m_vbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(verts)));
        m_vbuf->create();

        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
        m_ubuf->create();

        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_ubuf.get())});
        m_srb->create();

        const QString base = QCoreApplication::applicationDirPath() + "/shaders/";
        QShader vs = loadShader(base + "wedge.vert.qsb");
        QShader fs = loadShader(base + "wedge.frag.qsb");
        if (!vs.isValid() || !fs.isValid())
        {
            qCritical() << "spike_b: shaders missing under" << base;
            return;
        }

        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});

        m_ps.reset(m_rhi->newGraphicsPipeline());
        m_ps->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        m_ps->setVertexInputLayout(layout);
        m_ps->setShaderResourceBindings(m_srb.get());
        m_ps->setRenderPassDescriptor(m_rp.get());
        m_ps->create();

        QRhiResourceUpdateBatch* u = m_rhi->nextResourceUpdateBatch();
        u->uploadStaticBuffer(m_vbuf.get(), verts);
        m_pendingUpdates = u;
    }

    void render()
    {
        if (!m_rhi || !m_sc || !m_ps)
            return;
        if (!isExposed())
            return;

        if (m_sc->currentPixelSize() != m_sc->surfacePixelSize())
            m_sc->createOrResize();

        if (m_rhi->beginFrame(m_sc.get()) != QRhi::FrameOpSuccess)
            return;

        m_edr = queryEdrInfo(winId());

        QRhiResourceUpdateBatch* u = m_pendingUpdates ? m_pendingUpdates
                                                      : m_rhi->nextResourceUpdateBatch();
        m_pendingUpdates = nullptr;

        float ub[4] = {float(m_edr.maxColorComponent), 0.f, 0.f, 0.f};
        u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(ub), ub);

        QRhiCommandBuffer* cb = m_sc->currentFrameCommandBuffer();
        const QSize sz = m_sc->currentPixelSize();

        cb->beginPass(m_sc->currentFrameRenderTarget(), QColor(0, 0, 0), {1.0f, 0}, u);
        cb->setGraphicsPipeline(m_ps.get());
        cb->setViewport({0, 0, float(sz.width()), float(sz.height())});
        cb->setShaderResources(m_srb.get());
        const QRhiCommandBuffer::VertexInput vb(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &vb);
        cb->draw(3);
        cb->endPass();

        m_rhi->endFrame(m_sc.get());
    }

    void releaseRhi()
    {
        m_ps.reset();
        m_srb.reset();
        m_ubuf.reset();
        m_vbuf.reset();
        m_rp.reset();
        m_sc.reset();
        m_rhi.reset();
    }

    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_ps;
    QRhiResourceUpdateBatch* m_pendingUpdates = nullptr;
    EdrInfo m_edr;
    bool m_hdrSurface = false;
    QString m_formatName = "none";
};

//------------------------------------------------------------------------------
// RvDocument's arrangement: QStackedLayout(StackAll), GL widget added first,
// the native present surface added on top via createWindowContainer.
//------------------------------------------------------------------------------
class SpikeWindow : public QWidget
{
public:
    SpikeWindow()
    {
        m_gl = new GlUnderlay(this);
        m_metal = new MetalPresentWindow;
        m_container = QWidget::createWindowContainer(m_metal, this);

        // Match how GLView treats its overlay: never steal input.
        m_container->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_container->setFocusPolicy(Qt::NoFocus);

        m_stack = new QStackedLayout(this);
        m_stack->setStackingMode(QStackedLayout::StackAll);
        m_stack->addWidget(m_gl);
        m_stack->addWidget(m_container);

        setWindowTitle("Spike B — macOS EDR present surface");
        resize(1200, 600);

        // Drive repaints; also catches the case where the Metal surface stops
        // updating after a resize or display change.
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this] {
            if (m_metal->isExposed())
                m_metal->requestUpdate();
        });
        t->start(100);

        QTimer::singleShot(1200, this, [this] { report(); });
    }

    void report()
    {
        const EdrInfo e = m_metal->lastEdr();
        qInfo().noquote() << "\n================ Spike B result ================";
        qInfo().noquote() << "screen                :" << (e.valid ? e.screenName : "unknown");
        qInfo().noquote() << "swapchain format      :" << m_metal->formatName();
        qInfo().noquote() << "HDR surface active    :"
                          << (m_metal->hdrSurfaceActive() ? "YES" : "NO");
        qInfo().noquote() << "EDR headroom (now)    :" << e.maxColorComponent;
        qInfo().noquote() << "EDR headroom (max)    :" << e.maxPotential;
        qInfo().noquote() << "EDR reference         :" << e.maxReference;
        qInfo().noquote() << "container stacked over GL, size:" << m_container->size();
        qInfo().noquote() << "metal window exposed  :" << (m_metal->isExposed() ? "YES" : "NO");
        qInfo().noquote() << "===============================================\n";
        qInfo().noquote() << "Look at the panel: steps right of the 1.0 mark should get";
        qInfo().noquote() << "BRIGHTER, not stay flat white. Screenshots tone-map.";
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        switch (e->key())
        {
        case Qt::Key_H:
            m_container->setVisible(!m_container->isVisible());
            qInfo() << "spike_b: overlay visible =" << m_container->isVisible();
            break;
        case Qt::Key_F:
            isFullScreen() ? showNormal() : showFullScreen();
            break;
        case Qt::Key_I:
            report();
            break;
        case Qt::Key_Q:
            close();
            break;
        default:
            QWidget::keyPressEvent(e);
        }
    }

private:
    GlUnderlay* m_gl = nullptr;
    MetalPresentWindow* m_metal = nullptr;
    QWidget* m_container = nullptr;
    QStackedLayout* m_stack = nullptr;
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    SpikeWindow w;
    w.show();
    return app.exec();
}
