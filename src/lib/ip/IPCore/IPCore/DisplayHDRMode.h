//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//
// Which HDR display encoding the image pipeline emits, shared between the
// pipeline and the presentation layer.
//
// This has to live in IPCore because both readers need it and they sit in
// different libraries:
//
//   DisplayIPNode (IPCore)   decides what encoding goes *into* the buffer
//   GLView        (RvCommon) decides how the GL surface is tagged, and which
//                            present surface to create
//
// Rv::Options lives in RvApp, above IPCore, so it cannot be the home. Higher
// layers call the setter; everyone reads the getter.
//
// Seeded from RV_HDR / RV_HDR_ENCODING on first use so existing scripts and the
// env-var workflow keep working, then overridden by the preference.
//
// This is a deliberately thin stand-in for the descriptor-driven resolver in
// _hdr_test/HDR-SURFACE-DESIGN.md §3, which will eventually derive the encoding
// from the OCIO display colorspace rather than a fixed enum.
//******************************************************************************
#ifndef __IPCore__DisplayHDRMode__h__
#define __IPCore__DisplayHDRMode__h__

namespace IPCore
{

    enum class DisplayHDRMode
    {
        // No HDR: the existing SDR path, unchanged.
        Off = 0,

        // Display P3 primaries with the piecewise sRGB transfer, values free to
        // exceed 1.0. Pairs with an extended-linear Display P3 surface
        // (macOS EDR). The present shader decodes to linear.
        P3Extended = 1,

        // SMPTE-2084. Pairs with an HDR10 swapchain (Wayland).
        PQ = 2,
    };

    // Current mode. Thread-safe enough for this purpose: written from the UI
    // thread, read during graph evaluation and surface setup.
    DisplayHDRMode displayHDRMode();
    void setDisplayHDRMode(DisplayHDRMode);

    // True for any mode other than Off. Replaces the duplicated
    // wantHdrDisplay() helpers that read RV_HDR directly.
    bool displayHDREnabled();

    // The platform's natural mode, used when the user asks for HDR without
    // saying which encoding: P3Extended on macOS (Qt's Metal backend has no
    // HDR10 swapchain), PQ elsewhere.
    DisplayHDRMode defaultDisplayHDRMode();

} // namespace IPCore

#endif
