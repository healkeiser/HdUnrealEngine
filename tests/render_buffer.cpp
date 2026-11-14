#include "pxr/base/gf/half.h"
#include "delegate.h"
#include "depth.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <string_view>
#include <thread>
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            std::cout << "{\"map_cycles\":50,\"measurements\":[";
            bool comma = false;
            for (const GfVec3i size : {GfVec3i(1920, 1080, 1), GfVec3i(3840, 2160, 1)}) {
                HdUERenderBuffer image(SdfPath("/benchmark"));
                check(image.Allocate(size, HdFormatFloat32Vec4, false), "benchmark allocation failed");
                double checksum = 0;
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 50; ++i) {
                    const float* pixels = static_cast<const float*>(image.Map());
                    check(pixels != nullptr, "benchmark map failed");
                    checksum += pixels[(size_t(size[0]) * size[1] - 1) * 4];
                    image.Unmap();
                }
                const auto milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 50;
                if (comma) std::cout << ',';
                comma = true;
                std::cout << std::setprecision(9) << "{\"width\":" << size[0] << ",\"height\":" << size[1]
                          << ",\"map_unmap_mean_ms\":" << milliseconds << ",\"checksum\":" << checksum << '}';
            }
            std::cout << "]}\n";
            return 0;
        }
        HdUERenderBuffer buffer(SdfPath("/test"));
        check(!buffer.Allocate(GfVec3i(-1, 1, 1), HdFormatFloat32Vec4, false), "negative allocation accepted");
        check(!buffer.Allocate(GfVec3i(2, 1, 1), HdFormatUNorm8Vec4, false), "unsupported format accepted");
        {
            // The viewport colour AOV is half float: float images are encoded (and flipped) on publish.
            HdUERenderBuffer half(SdfPath("/half"));
            check(half.Allocate(GfVec3i(1, 2, 1), HdFormatFloat16Vec4, false), "half-float allocation rejected");
            HdueFrameInfo info{};
            info.width = 1;
            info.height = 2;
            half.publish(info, {0.5f, 1.f, 2.f, 1.f, 3.f, 0.f, 0.f, 1.f});
            const auto* texels = static_cast<const GfHalf*>(half.Map());
            check(texels && float(texels[0]) == 3.f && float(texels[4]) == 0.5f && float(texels[6]) == 2.f,
                  "half-float image not encoded bottom-up");
            half.Unmap();
        }
        // Large frames (a publish copies and flips 1 MB) and a common start, so publishing and reading overlap: with
        // 2x1 frames the publisher had finished before the reader looked. Each publisher keeps going until the reader
        // has made its minimum of passes, so those passes all race a publish.
        constexpr int side = 256, minimumReads = 50;
        const size_t values = size_t(side) * side * 4;
        check(buffer.Allocate(GfVec3i(side, side, 1), HdFormatFloat32Vec4, false), "valid allocation rejected");
        HdueFrameInfo frame{};
        frame.width = frame.height = side;
        frame.converged = 1;
        buffer.publish(frame, std::vector<float>(values, 1));
        const auto* pinned = static_cast<const float*>(buffer.Map());
        check(pinned && pinned[0] == 1, "initial image missing");
        check(!buffer.Allocate(GfVec3i(3, 2, 1), HdFormatFloat32Vec4, false), "mapped image was reallocated");
        std::atomic<bool> start = false, publishing = true;
        std::atomic<int> reads = 0;
        std::thread publisher([&] {
            while (!start) std::this_thread::yield();
            for (int i = 0; i < 100 || reads < minimumReads; ++i) buffer.publish(frame, std::vector<float>(values, 2));
            publishing = false;
        });
        start = true;
        // The pinned image must not change under the reader while frames are published.
        for (; publishing; ++reads)
            for (size_t i = 0; i < values; ++i) check(pinned[i] == 1, "published frames modified mapped memory");
        publisher.join();
        for (size_t i = 0; i < values; ++i) check(pinned[i] == 1, "published frames modified mapped memory");
        buffer.Unmap();
        const auto* fresh = static_cast<const float*>(buffer.Map());
        for (size_t i = 0; i < values; ++i) check(fresh[i] == 2, "next Map did not see the completed frame");
        buffer.Unmap();
        // Unpinned maps racing publication see one whole frame, never a mix of two (publish recycles allocations).
        start = false;
        publishing = true;
        reads = 0;
        std::thread alternating([&] {
            while (!start) std::this_thread::yield();
            for (int i = 0; i < 200 || reads < minimumReads; ++i)
                buffer.publish(frame, std::vector<float>(values, float(1 + i % 2)));
            publishing = false;
        });
        start = true;
        for (; publishing; ++reads) {
            const auto* image = static_cast<const float*>(buffer.Map());
            check(image && (image[0] == 1 || image[0] == 2), "racing Map returned no frame");
            for (size_t i = 1; i < values; ++i) check(image[i] == image[0], "racing Map mixed two frames");
            buffer.Unmap();
        }
        alternating.join();
        buffer.invalidate();
        check(!buffer.IsConverged(), "new generation kept old convergence");
        check(buffer.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32Vec4, false), "2D allocation failed");
        frame.width = frame.height = 2;
        std::vector<float> asymmetric(16, 0);
        asymmetric[0] = 3;
        asymmetric[8] = 7;
        buffer.publish(frame, std::move(asymmetric));
        const auto* oriented = static_cast<const float*>(buffer.Map());
        check(oriented[0] == 7 && oriented[8] == 3, "UE top-down image was not converted to Hydra lower-left AOV");
        const auto* nested = static_cast<const float*>(buffer.Map());
        check(nested == oriented, "nested Map changed the pinned allocation");
        buffer.publish(frame, std::vector<float>(16, 9));
        buffer.Unmap();
        check(buffer.IsMapped() && oriented[0] == 7, "partial Unmap released a live snapshot");
        buffer.Finalize(nullptr);
        check(oriented[0] == 7 && buffer.Map() == oriented, "deallocation invalidated a live mapped snapshot");
        buffer.Unmap();
        buffer.Unmap();
        check(!buffer.IsMapped() && buffer.Map() == nullptr, "deallocated buffer exposed stale pixels");
        check(buffer.Allocate(GfVec3i(1, 1, 1), HdFormatFloat32Vec4, false),
              "reallocation after snapshot release failed");
        check(static_cast<const float*>(buffer.Map())[0] == 0, "reallocation retained stale image data");
        buffer.Unmap();
        check(buffer.Allocate(GfVec3i(2, 1, 1), HdFormatFloat32Vec4, true), "progressive AOV allocation rejected");
        check(buffer.IsMultiSampled(), "multiSampled descriptor was lost");
        frame.height = 1;
        buffer.publish(frame, std::vector<float>(8, 5));
        buffer.Resolve();
        check(static_cast<const float*>(buffer.Map())[0] == 5, "resolved progressive image missing");
        buffer.Unmap();
        HdUERenderBuffer depth(SdfPath("/depth"));
        check(depth.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32, false), "scalar depth allocation rejected");
        check(depth.GetFormat() == HdFormatFloat32, "depth format lost");
        frame.height = 2;
        depth.publish(frame, {.1f, .2f, .7f, 1.f});
        const auto* z = static_cast<const float*>(depth.Map());
        check(z[0] == .7f && z[1] == 1.f && z[2] == .1f && z[3] == .2f, "depth row orientation incorrect");
        depth.Unmap();
        // A color3f beauty receives RGBA; an RGBA buffer receives RGB with alpha 1.
        HdUERenderBuffer rgb(SdfPath("/rgb"));
        check(rgb.Allocate(GfVec3i(1, 1, 1), HdFormatFloat32Vec3, false), "color3f allocation rejected");
        frame.height = 1;
        frame.width = 1;
        rgb.publish(frame, {.1f, .2f, .3f, .4f}, 4);
        const auto* c = static_cast<const float*>(rgb.Map());
        check(c[0] == .1f && c[1] == .2f && c[2] == .3f, "RGBA to RGB conversion incorrect");
        rgb.Unmap();
        HdUERenderBuffer rgba(SdfPath("/rgba"));
        check(rgba.Allocate(GfVec3i(1, 1, 1), HdFormatFloat32Vec4, false), "color4f allocation rejected");
        rgba.publish(frame, {.5f, .6f, .7f}, 3);
        const auto* a = static_cast<const float*>(rgba.Map());
        check(a[0] == .5f && a[2] == .7f && a[3] == 1.f, "RGB to RGBA conversion incorrect");
        rgba.Unmap();
        GfMatrix4d projection(0.);
        const double nearPlane = 1, farPlane = 1000;
        projection[0][0] = projection[1][1] = 1;
        projection[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        projection[2][3] = -1;
        projection[3][2] = -2 * farPlane * nearPlane / (farPlane - nearPlane);
        check(HdUEProjectDepth(0, projection, 1) == 1, "background depth must be far");
        check(std::abs(HdUEProjectDepth(1, projection, 1)) < 1e-6, "near depth projection incorrect");
        check(std::abs(HdUEProjectDepth(1000, projection, 1) - 1) < 1e-6, "far depth projection incorrect");
        check(std::abs(HdUEProjectDepth(10, projection, 1) - .9009009f) < 1e-6,
              "perspective depth projection incorrect");
        check(std::abs(HdUEProjectDepth(1000, projection, 100) - .9009009f) < 1e-6,
              "meter stage depth conversion incorrect");
        std::cout << "PASS: stable mapped snapshots, concurrent publication, allocation and convergence\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
