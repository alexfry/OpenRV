//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// EDR capability query for the display a window is on.
//
// Deliberately does NOT use QRhiSwapChain::hdrInfo(). On Metal, Qt reports
// ColorComponentValue / DisplayReferred with a *hardcoded dummy* sdrWhiteLevel
// of 200 (qrhimetal.mm, hdrInfo ~6403), so consuming it would be misleading.
// The real numbers come from NSScreen.
//******************************************************************************
#ifndef SPIKE_B_EDRINFO_H
#define SPIKE_B_EDRINFO_H

#include <QtGlobal>
#include <QString>

struct EdrInfo
{
    // NSScreen.maximumExtendedDynamicRangeColorComponentValue — how far above
    // 1.0 the display will actually show *right now*. 1.0 means no EDR.
    double maxColorComponent = 1.0;

    // maximumPotentialExtendedDynamicRangeColorComponentValue — the ceiling if
    // conditions allow (brightness, thermals, other EDR content on screen).
    double maxPotential = 1.0;

    // maximumReferenceExtendedDynamicRangeColorComponentValue — reference-mode
    // headroom, non-zero on XDR panels in a reference preset.
    double maxReference = 0.0;

    QString screenName;
    bool valid = false;
};

// winId is a WId from QWindow::winId() (an NSView* on macOS).
EdrInfo queryEdrInfo(quintptr winId);

#endif
