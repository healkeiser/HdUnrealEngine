#include "volume_fields.h"
#include <GU/GU_Detail.h>
#include <GU/GU_DetailHandle.h>
#include <GEO/GEO_PrimVDB.h>
#include <GEO/GEO_PrimVolume.h>
#include <GA/GA_Handle.h>
#include <UT/UT_VoxelArray.h>
#include <HUSD/XUSD_LockedGeoRegistry.h>
#include "pxr/usd/sdf/layer.h"
#include <stdexcept>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
// Local voxel i covers [i, i+1]; its sample is index first + stride * i (voxel centres at integer indices).
GfMatrix4d HdUELocalToIndex(int stride, const GfVec3d& first) {
    return GfMatrix4d(1).SetScale(double(stride)) * GfMatrix4d(1).SetTranslate(first - GfVec3d(.5 * stride));
}
int HdUEStride(int x, int y, int z, int maxResolution) {
    return std::max({1, (x + maxResolution - 1) / maxResolution, (y + maxResolution - 1) / maxResolution, (z + maxResolution - 1) / maxResolution});
}
// Solaris SOP layers: op:<sop path>.sop.<kind>:SDF_FORMAT_ARGS:<args>. The LOP that imported them keeps the
// cooked detail in XUSD_LockedGeoRegistry; otherwise ask Houdini to cook the SOP through its op: file reader.
GU_ConstDetailHandle HdUEFindSopGeometry(const std::string& path) {
    std::string layer;
    SdfLayer::FileFormatArguments args;
    SdfLayer::SplitIdentifier(path, &layer, &args);
    std::string node = layer.substr(3);
    if (const auto sop = node.rfind(".sop"); sop != std::string::npos) node.erase(sop);
    for (const auto& candidate : {layer.substr(3), node, layer})
        if (auto gdh = XUSD_LockedGeoRegistry::getGeometry(UT_StringHolder(candidate), args); gdh.isValid()) return gdh;
    auto* detail = new GU_Detail;
    GU_DetailHandle handle; handle.allocateAndSet(detail);
    if (!detail->load(("op:" + node).c_str()).success()) return {};
    return handle;
}
}

void HdUEDensifyGrid(const openvdb::FloatGrid& grid, int maxResolution, HdUEVolumeData& volume) {
    const auto box = grid.evalActiveVoxelBoundingBox();
    if (box.empty()) return;
    const auto size = box.dim();
    const int stride = HdUEStride(size.x(), size.y(), size.z(), maxResolution);
    volume.width = uint32_t((size.x() + stride - 1) / stride); volume.height = uint32_t((size.y() + stride - 1) / stride); volume.depth = uint32_t((size.z() + stride - 1) / stride);
    volume.density.resize(size_t(volume.width) * volume.height * volume.depth);
    auto accessor = grid.getConstAccessor();
    const auto min = box.min();
    for (uint32_t k = 0; k < volume.depth; ++k) for (uint32_t j = 0; j < volume.height; ++j) for (uint32_t i = 0; i < volume.width; ++i)
        volume.density[(size_t(k) * volume.height + j) * volume.width + i] =
            accessor.getValue(openvdb::Coord(min.x() + int(i) * stride, min.y() + int(j) * stride, min.z() + int(k) * stride));
    const auto affine = grid.transform().baseMap()->getAffineMap();
    if (!affine) throw std::runtime_error("grid " + grid.getName() + " has a non-affine (frustum) transform");
    const auto m = affine->getMat4();
    GfMatrix4d indexToWorld; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) indexToWorld[r][c] = m(r, c);
    volume.localToWorld = HdUELocalToIndex(stride, GfVec3d(min.x(), min.y(), min.z())) * indexToWorld;
}

void HdUELoadHoudiniField(const std::string& path, const std::string& name, int index, bool vdb, int maxResolution, HdUEVolumeData& volume) {
    GU_ConstDetailHandle handle;
    if (path.rfind("op:", 0) == 0) handle = HdUEFindSopGeometry(path);
    else {
        auto* detail = new GU_Detail;
        GU_DetailHandle owned; owned.allocateAndSet(detail);
        if (detail->load(path.c_str()).success()) handle = owned;
    }
    GU_DetailHandleAutoReadLock lock(handle);
    const GU_Detail* detail = lock.getGdp();
    if (!detail) throw std::runtime_error("cannot read Houdini geometry " + path);
    // The named primitive of the requested kind, else the index-th one of that kind.
    const GA_ROHandleS names(detail, GA_ATTRIB_PRIMITIVE, "name");
    const GEO_Primitive* named = nullptr;
    std::vector<const GEO_Primitive*> kind;
    for (GA_Iterator it(detail->getPrimitiveRange()); !it.atEnd(); ++it) {
        const auto* prim = detail->getGEOPrimitive(*it);
        if (prim->getTypeId() != (vdb ? GEO_PRIMVDB : GEO_PRIMVOLUME)) continue;
        kind.push_back(prim);
        if (!named && !name.empty() && names.isValid() && names.get(*it) == name.c_str()) named = prim;
    }
    const GEO_Primitive* chosen = named ? named : index >= 0 && size_t(index) < kind.size() ? kind[size_t(index)] : kind.empty() ? nullptr : kind.front();
    if (!chosen) throw std::runtime_error(std::string("no ") + (vdb ? "VDB" : "Houdini volume") + " primitive " + name + " in " + path);
    if (vdb) {
        const auto grid = openvdb::gridConstPtrCast<openvdb::FloatGrid>(static_cast<const GEO_PrimVDB*>(chosen)->getConstGridPtr());
        if (!grid) throw std::runtime_error("VDB " + name + " in " + path + " is not a float grid");
        HdUEDensifyGrid(*grid, maxResolution, volume);
        return;
    }
    const auto* field = static_cast<const GEO_PrimVolume*>(chosen);
    if (field->getTaperX() != 1 || field->getTaperY() != 1) throw std::runtime_error("tapered (frustum) Houdini volume " + name + " is not supported");
    int rx = 0, ry = 0, rz = 0; field->getRes(rx, ry, rz);
    if (rx <= 0 || ry <= 0 || rz <= 0) return;
    const int stride = HdUEStride(rx, ry, rz, maxResolution);
    volume.width = uint32_t((rx + stride - 1) / stride); volume.height = uint32_t((ry + stride - 1) / stride); volume.depth = uint32_t((rz + stride - 1) / stride);
    volume.density.resize(size_t(volume.width) * volume.height * volume.depth);
    const UT_VoxelArrayReadHandleF voxels(field->getVoxelHandle());
    for (uint32_t k = 0; k < volume.depth; ++k) for (uint32_t j = 0; j < volume.height; ++j) for (uint32_t i = 0; i < volume.width; ++i)
        volume.density[(size_t(k) * volume.height + j) * volume.width + i] = (*voxels)(int(i) * stride, int(j) * stride, int(k) * stride);
    // Voxel centres are affine in their index: rows are the index axes and the first centre.
    UT_Vector3 origin, x, y, z;
    field->indexToPos(0, 0, 0, origin); field->indexToPos(1, 0, 0, x); field->indexToPos(0, 1, 0, y); field->indexToPos(0, 0, 1, z);
    GfMatrix4d indexToWorld(1);
    for (int c = 0; c < 3; ++c) {
        indexToWorld[0][c] = x[c] - origin[c]; indexToWorld[1][c] = y[c] - origin[c]; indexToWorld[2][c] = z[c] - origin[c]; indexToWorld[3][c] = origin[c];
    }
    volume.localToWorld = HdUELocalToIndex(stride, GfVec3d(0)) * indexToWorld;
}
PXR_NAMESPACE_CLOSE_SCOPE
