#pragma once
#include "delegate.h"
#include <openvdb/openvdb.h>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE
// Densifies a float grid over its active bounding box into volume (density, size, localToWorld in grid
// world space); grids above maxResolution voxels per axis are point-sampled with a stride.
void HdUEDensifyGrid(const openvdb::FloatGrid& grid, int maxResolution, HdUEVolumeData& volume);
// Reads a field from Houdini geometry: the SOP cook behind an op: layer path (Solaris, in-process), or a
// .bgeo/.bgeo.sc/.vdb file (what the USD Render ROP writes for husk). It takes the VDB (vdb) or native
// Houdini volume primitive called name, or the index-th one when no primitive has that name.
// localToWorld is in SOP space. Throws std::runtime_error when the geometry or field is missing.
void HdUELoadHoudiniField(const std::string& path, const std::string& name, int index, bool vdb, int maxResolution,
                          HdUEVolumeData& volume);
PXR_NAMESPACE_CLOSE_SCOPE
