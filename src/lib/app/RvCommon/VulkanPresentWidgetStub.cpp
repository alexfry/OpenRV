//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Non-Linux stub for the Vulkan/QRhi present surface.
//
// The real implementation (VulkanPresentWidget.cpp + GlVkSharedImage.cpp) is
// Linux-only: it includes <vulkan/vulkan.h> and shares GL↔Vk images through
// POSIX fd external memory, which does not exist on macOS or Windows. The macOS
// equivalent is IOSurface-based and will be a separate backend — see
// _hdr_test/HDR-SURFACE-DESIGN.md.
//
// Nothing here is ever reached at runtime. RvDocument only constructs a
// VulkanPresentWidget when the Qt platform plugin name starts with "wayland"
// (or RV_VULKAN_PRESENT forces it), so off Linux no instance exists and
// GLView's qobject_cast<VulkanPresentWidget*> returns nullptr, leaving every
// `if (vk && ...)` present branch inert. These definitions exist purely so that
// GLView.cpp and RvDocument.cpp link without carrying platform #ifdefs.
//******************************************************************************

#include <RvCommon/VulkanPresentWidget.h>

// VulkanPresentWindow holds std::unique_ptr members of QRhi types that the
// header only forward-declares, so its constructor and destructor need the
// complete types even though this stub never allocates any of them. qrhi.h is
// Qt's versioned private-style header; the include path is set up in
// CMakeLists.txt alongside the one the real Linux implementation uses. Note
// this pulls in no Vulkan headers — QRhi itself is backend-agnostic.
#include <rhi/qrhi.h>

// Same reason: m_shared is a unique_ptr<GlVkSharedImage>. This header is also
// Vulkan-free; only GlVkSharedImage.cpp pulls in <vulkan/vulkan.h>.
#include <RvCommon/GlVkSharedImage.h>

// GlVkSharedImage holds a unique_ptr to this opaque struct, which the real
// implementation defines in GlVkSharedImage.cpp (where it wraps VkImage,
// VkDeviceMemory, …). That file is not compiled off Linux, so this stub owns
// the one and only definition in the program — no ODR conflict.
struct GlVkSharedImageNative
{
};

namespace Rv
{

    // Needed because ~VulkanPresentWindow destroys a unique_ptr<GlVkSharedImage>.
    GlVkSharedImage::~GlVkSharedImage() = default;

    VulkanPresentWindow::VulkanPresentWindow() = default;
    VulkanPresentWindow::~VulkanPresentWindow() = default;

    void VulkanPresentWindow::setFrame(QImage) {}

    void VulkanPresentWindow::setFrameHalf(int, int, std::vector<uint16_t>) {}

    void VulkanPresentWindow::setHdrPresent(bool) {}

    // False keeps GLView on the plain 8-bit present path: no RGBA16F FBO is
    // allocated and no float readback happens.
    bool VulkanPresentWindow::presentModeNeedsFloatTransfer() { return false; }

    bool VulkanPresentWindow::usingGpuInterop() const { return false; }

    bool VulkanPresentWindow::ensureGpuInterop(QOpenGLContext*, const QSize&, bool) { return false; }

    bool VulkanPresentWindow::blitFromGlFramebuffer(unsigned int, int, int) { return false; }

    void VulkanPresentWindow::presentGpuInteropFrame() {}

    void VulkanPresentWindow::exposeEvent(QExposeEvent*) {}

    void VulkanPresentWindow::resizeEvent(QResizeEvent*) {}

    bool VulkanPresentWindow::event(QEvent* e) { return QWindow::event(e); }

    VulkanPresentWidget::VulkanPresentWidget(QWidget* parent)
        : QWidget(parent)
    {
    }

    VulkanPresentWidget::~VulkanPresentWidget() = default;

    void VulkanPresentWidget::setFrame(QImage) {}

    void VulkanPresentWidget::setFrameHalf(int, int, std::vector<uint16_t>) {}

    void VulkanPresentWidget::setHdrPresent(bool) {}

    bool VulkanPresentWidget::hdrPresent() const { return false; }

    bool VulkanPresentWidget::usingGpuInterop() const { return false; }

    bool VulkanPresentWidget::ensureGpuInterop(QOpenGLContext*, const QSize&, bool) { return false; }

    bool VulkanPresentWidget::blitFromGlFramebuffer(unsigned int, int, int) { return false; }

    void VulkanPresentWidget::presentGpuInteropFrame() {}

} // namespace Rv
