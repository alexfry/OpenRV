// Full-frame HDR wedge for OpenRV RV_HDR=1 verification.
// Scene-linear patches; DisplayIPNode PQ treats 1.0 = 100 nits.
//
// Layout (1920x1080, fills a 16:9 viewer with no pillarbox):
//   Left third  (0..1/3):   1.0  →  100 nits
//   Center third(1/3..2/3): 10.0 → 1000 nits
//   Right third (2/3..1):   4.0  →  400 nits
//   Bottom strip (y last 8%): stepped ramp 0.01, 0.1, 0.18, 1, 2, 4, 10
//
// Probes at x=20%/50%/80% mid-height should read PQ codes ≈130 / 192 / 164.

#include <ImfRgbaFile.h>
#include <ImfArray.h>
#include <ImfHeader.h>
#include <iostream>
#include <cstring>

using namespace OPENEXR_IMF_NAMESPACE;
using namespace IMATH_NAMESPACE;

static float rampValue(int x, int w)
{
    // 7 equal steps across width
    const float steps[] = {0.01f, 0.10f, 0.18f, 1.0f, 2.0f, 4.0f, 10.0f};
    const int n = 7;
    int i = (x * n) / w;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return steps[i];
}

int main(int argc, char** argv)
{
    const char* path =
        argc > 1 ? argv[1] : "/home/alex/github/openrv/_hdr_test/hdr_wedge_1080.exr";
    // Optional legacy path for the old name.
    const bool alsoLegacy =
        (argc <= 1); // default write also overwrites step1 for convenience

    const int w = 1920;
    const int h = 1080;
    const int third = w / 3;
    const int rampH = h * 8 / 100; // bottom 8%

    Array2D<Rgba> px(h, w);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float v;
            if (y >= h - rampH)
            {
                v = rampValue(x, w);
            }
            else if (x < third)
            {
                v = 1.0f; // 100 nits
            }
            else if (x < 2 * third)
            {
                v = 10.0f; // 1000 nits
            }
            else
            {
                v = 4.0f; // 400 nits
            }
            px[y][x].r = px[y][x].g = px[y][x].b = v;
            px[y][x].a = 1.0f;
        }
    }

    auto writeOne = [&](const char* outPath) {
        Header header(w, h);
        RgbaOutputFile file(outPath, header, WRITE_RGBA);
        file.setFrameBuffer(&px[0][0], 1, w);
        file.writePixels(h);
        std::cout << "Wrote " << outPath << " (" << w << "x" << h << ")\n";
    };

    writeOne(path);
    if (alsoLegacy)
    {
        writeOne("/home/alex/github/openrv/_hdr_test/hdr_white_step1.exr");
    }

    std::cout
        << "  Left third  x=0.." << third << ":      1.0  (100 nits)\n"
        << "  Center third x=" << third << ".." << (2 * third) << ": 10.0 (1000 nits)\n"
        << "  Right third x=" << (2 * third) << ".." << w << ":  4.0  (400 nits)\n"
        << "  Bottom ramp: 0.01 / 0.1 / 0.18 / 1 / 2 / 4 / 10\n"
        << "  Expected LCR8 probes @20/50/80%: ~130 / ~192 / ~164\n";
    return 0;
}
