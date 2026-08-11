//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include "edrinfo.h"

#import <AppKit/AppKit.h>

EdrInfo queryEdrInfo(quintptr winId)
{
    EdrInfo info;

    @autoreleasepool
    {
        NSScreen* screen = nil;

        // QWindow::winId() is the NSView on macOS. Prefer the screen that view
        // is actually on — headroom is per-display, and dragging the window to
        // a different monitor changes it.
        if (winId)
        {
            id obj = reinterpret_cast<id>(winId);
            if ([obj isKindOfClass:[NSView class]])
            {
                NSView* view = static_cast<NSView*>(obj);
                screen = view.window.screen;
            }
        }

        if (!screen)
            screen = [NSScreen mainScreen];
        if (!screen)
            return info;

        info.maxColorComponent = screen.maximumExtendedDynamicRangeColorComponentValue;
        info.maxPotential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

        if ([screen respondsToSelector:@selector
                    (maximumReferenceExtendedDynamicRangeColorComponentValue)])
        {
            info.maxReference = screen.maximumReferenceExtendedDynamicRangeColorComponentValue;
        }

        info.screenName = QString::fromNSString(screen.localizedName);
        info.valid = true;
    }

    return info;
}
