//******************************************************************************
// SPDX-License-Identifier: Apache-2.0
//******************************************************************************

#include <IPCore/DisplayHDRMode.h>

#include <cstdlib>
#include <cstring>

namespace IPCore
{
    namespace
    {
        bool envTrue(const char* v)
        {
            return v && *v && strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "off")
                   && strcasecmp(v, "no");
        }

        DisplayHDRMode modeFromEnv()
        {
            if (!envTrue(getenv("RV_HDR")))
                return DisplayHDRMode::Off;

            const char* enc = getenv("RV_HDR_ENCODING");
            if (enc && *enc)
            {
                if (!strcasecmp(enc, "p3extended") || !strcasecmp(enc, "p3"))
                    return DisplayHDRMode::P3Extended;
                if (!strcasecmp(enc, "pq") || !strcasecmp(enc, "st2084"))
                    return DisplayHDRMode::PQ;
            }
            return defaultDisplayHDRMode();
        }

        // Seeded lazily so the env vars are read once, after main() has set up
        // the environment but before the first graph evaluation.
        DisplayHDRMode& modeRef()
        {
            static DisplayHDRMode mode = modeFromEnv();
            return mode;
        }
    } // namespace

    DisplayHDRMode defaultDisplayHDRMode()
    {
#ifdef PLATFORM_DARWIN
        // Qt's Metal backend exposes no HDR10/PQ swapchain, so PQ on macOS
        // means encoding to something nothing can present.
        return DisplayHDRMode::P3Extended;
#else
        return DisplayHDRMode::PQ;
#endif
    }

    DisplayHDRMode displayHDRMode() { return modeRef(); }

    void setDisplayHDRMode(DisplayHDRMode m) { modeRef() = m; }

    bool displayHDREnabled() { return modeRef() != DisplayHDRMode::Off; }

} // namespace IPCore
