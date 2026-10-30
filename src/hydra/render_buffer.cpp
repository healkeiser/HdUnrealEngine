#include "delegate.h"
#include "pxr/base/work/loops.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include <algorithm>
#include <cstring>
PXR_NAMESPACE_OPEN_SCOPE
namespace {
std::mutex liveMutex;
std::set<const HdRenderBuffer*> live;
}  // namespace
HdUERenderBuffer::HdUERenderBuffer(const SdfPath& id) : HdRenderBuffer(id) {
    std::lock_guard lock(liveMutex);
    live.insert(this);
}
HdUERenderBuffer::~HdUERenderBuffer() {
    std::lock_guard lock(liveMutex);
    live.erase(this);
}
bool HdUERenderBuffer::alive(const HdRenderBuffer* b) {
    std::lock_guard lock(liveMutex);
    return b && live.count(b);
}
bool HdUERenderBuffer::Allocate(const GfVec3i& dimensions, HdFormat format, bool multi) {
    std::lock_guard lock(mutex_);
    if (maps_ || dimensions[0] < 1 || dimensions[1] < 1 || dimensions[0] > 8192 || dimensions[1] > 8192 ||
        dimensions[2] != 1 ||
        (format != HdFormatFloat32Vec4 && format != HdFormatFloat16Vec4 && format != HdFormatFloat32Vec3 &&
         format != HdFormatFloat32 && format != HdFormatInt32)) {
        allocationError_ = std::string(dimensions[0] > 8192 || dimensions[1] > 8192
                                           ? "images are limited to 8192 pixels per axis (render larger ones in tiles: "
                                             "husk --tile-count); "
                                           : "") +
                           "dimensions=" + std::to_string(dimensions[0]) + "," + std::to_string(dimensions[1]) + "," +
                           std::to_string(dimensions[2]) + " format=" + std::to_string(int(format)) +
                           " multisampled=" + std::to_string(multi) + " maps=" + std::to_string(maps_);
        return false;
    }
    allocationError_.clear();
    width_ = dimensions[0];
    height_ = dimensions[1];
    format_ = format;
    // UE publishes already resolved progressive images. Map and Resolve see
    // that same completed image, including Houdini's multiSampled request.
    multiSampled_ = multi;
    pixels_ =
        std::make_shared<std::vector<uint8_t>>(size_t(width_) * height_ * HdDataSizeOfFormat(format_), uint8_t(0));
    converged_ = false;
    return true;
}
unsigned HdUERenderBuffer::GetWidth() const {
    std::lock_guard lock(mutex_);
    return width_;
}
unsigned HdUERenderBuffer::GetHeight() const {
    std::lock_guard lock(mutex_);
    return height_;
}
void* HdUERenderBuffer::Map() {
    std::lock_guard lock(mutex_);
    if ((!pixels_ || pixels_->empty()) && !maps_) return nullptr;
    // Pin the immutable published allocation. A newer frame replaces pixels_
    // without copying this image or invalidating any active Map pointers.
    if (maps_++ == 0) pinned_ = pixels_;
    return pinned_->data();
}
void HdUERenderBuffer::Unmap() {
    std::lock_guard lock(mutex_);
    if (maps_ && --maps_ == 0) pinned_.reset();
}
bool HdUERenderBuffer::IsMapped() const {
    std::lock_guard lock(mutex_);
    return maps_ > 0;
}
VtValue HdUERenderBuffer::GetResource(bool) const {
    std::function<VtValue()> resource;
    {
        std::lock_guard lock(mutex_);
        resource = resource_;
    }
    return resource ? resource() : VtValue();
}
bool HdUERenderBuffer::IsConverged() const {
    std::lock_guard lock(mutex_);
    return converged_;
}
void HdUERenderBuffer::invalidate() {
    std::lock_guard lock(mutex_);
    converged_ = false;
}
void HdUERenderBuffer::_Deallocate() {
    std::lock_guard lock(mutex_);
    pixels_.reset();
    if (!maps_) pinned_.reset();
    width_ = height_ = 0;
    converged_ = false;
}
void HdUERenderBuffer::publish(const HdueFrameInfo& frame, std::vector<float> pixels, size_t sourceChannels) {
    std::lock_guard lock(mutex_);
    const size_t channels = HdGetComponentCount(format_), bytes = HdDataSizeOfFormat(format_);
    if (!sourceChannels) sourceChannels = channels;
    if (frame.width != width_ || frame.height != height_ ||
        pixels.size() != size_t(width_) * height_ * sourceChannels) {
        if (!sizeWarned_)
            TF_WARN("hdUE: %s receives a %ux%u image with %zu channels for its %ux%u buffer; frame dropped",
                    GetId().GetText(), frame.width, frame.height, sourceChannels, width_, height_);
        sizeWarned_ = true;
        return;
    }
    // A buffer allocated with other channels than hdUE renders (a color3f beauty, the UsdRenderVar default; an RGBA
    // albedo): copy the common channels, fill the rest (alpha 1).
    if (sourceChannels != channels) {
        std::vector<float> converted(size_t(width_) * height_ * channels);
        WorkParallelForN(size_t(width_) * height_, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                for (size_t c = 0; c < channels; ++c)
                    converted[i * channels + c] = c < sourceChannels ? pixels[i * sourceChannels + c]
                                                  : c == 3           ? 1.f
                                                                     : 0.f;
        });
        pixels.swap(converted);
    }
    sizeWarned_ = false;
    std::vector<uint8_t> out;
    out.swap(spareBytes_);
    out.resize(size_t(width_) * height_ * bytes);
    // UE's C ABI is top-down; Hydra/Hdx CPU AOVs use a lower-left origin. Flip while encoding, one pass.
    const bool half = HdGetComponentFormat(format_) == HdFormatFloat16;
    const size_t row = size_t(width_) * channels;
    WorkParallelForN(height_, [&](size_t begin, size_t end) {
        for (size_t y = begin; y < end; ++y) {
            const float* source = pixels.data() + y * row;
            uint8_t* target = out.data() + (height_ - 1 - y) * row * (bytes / channels);
            if (half)
                for (size_t i = 0; i < row; ++i) reinterpret_cast<GfHalf*>(target)[i] = GfHalf(source[i]);
            else
                std::memcpy(target, source, row * sizeof(float));
        }
    });
    if (pixels_ && pixels_.use_count() == 1) spareBytes_.swap(*pixels_);
    pixels_ = std::make_shared<std::vector<uint8_t>>(std::move(out));
    spare_ = std::move(pixels);
    converged_ = frame.converged != 0;
}
std::vector<float> HdUERenderBuffer::takeSpare(size_t size) {
    std::vector<float> spare;
    {
        std::lock_guard lock(mutex_);
        spare.swap(spare_);
    }
    spare.resize(size);
    return spare;
}
PXR_NAMESPACE_CLOSE_SCOPE
