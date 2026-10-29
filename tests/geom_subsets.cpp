#include "geom_subsets.h"
#include "mesh_normals.h"
#include "mesh_weld.h"
#include "wet_mask.h"
#include "pxr/imaging/hd/tokens.h"
#include <iostream>
#include <limits>

PXR_NAMESPACE_USING_DIRECTIVE
static void require(bool condition) { if (!condition) throw std::runtime_error("GeomSubset contract failed"); }
int main() {
    try {
        HdMeshTopology topology(TfToken("none"),HdTokens->rightHanded,
            VtIntArray{4,3,3},VtIntArray{0,1,2,3,4,5,6,7,8,9});
        HdGeomSubset subset; subset.type=HdGeomSubset::TypeFaceSet;
        subset.id=SdfPath("/Mesh/Quad"); subset.materialId=SdfPath("/Looks/Red"); subset.indices={0};
        topology.SetGeomSubsets({subset});
        VtVec3iArray triangles; VtIntArray params;
        HdMeshUtil util(&topology,SdfPath("/Mesh")); util.ComputeTriangleIndices(&triangles,&params);
        const auto constant=HdUEExpandWetMask(VtValue(.5f),HdInterpolationConstant,10,topology,triangles,params,util);
        require(constant==std::vector<float>(triangles.size()*3,.5f));
        for (auto domain:{HdInterpolationVertex,HdInterpolationVarying,HdInterpolationFaceVarying}) {
            VtFloatArray samples; for (int i=0;i<10;++i) samples.push_back(i/10.f);
            const auto expanded=HdUEExpandWetMask(VtValue(samples),domain,10,topology,triangles,params,util);
            for (size_t i=0;i<expanded.size();++i) require(std::abs(expanded[i]-triangles[i/3][i%3]/10.f)<1e-6f);
        }
        const auto uniform=HdUEExpandWetMask(VtValue(VtFloatArray{-.5f,.5f,2.f}),HdInterpolationUniform,10,topology,triangles,params,util);
        for (size_t tri=0;tri<triangles.size();++tri) for (int c:{0,1,2})
            require(uniform[tri*3+c]==std::clamp(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(params[tri])*.5f,0.f,1.f));
        for (const auto& invalid:{VtValue(VtFloatArray{.5f}),VtValue(VtFloatArray(10,std::numeric_limits<float>::quiet_NaN())),VtValue(1)}) {
            bool rejected=false;
            try {HdUEExpandWetMask(invalid,HdInterpolationVertex,10,topology,triangles,params,util);} catch(const std::invalid_argument&) {rejected=true;}
            require(rejected);
        }
        // Per-point attributes weld to one vertex per point; a face-varying value that differs splits it.
        std::vector<uint32_t> indices;
        const auto welded=HdUEWeldCorners(triangles,10,[](size_t,size_t){return true;},indices);
        require(welded.size()==10 && indices.size()==triangles.size()*3);
        for (size_t corner=0;corner<indices.size();++corner) require(triangles[welded[indices[corner]]/3][welded[indices[corner]]%3]==triangles[corner/3][corner%3]);
        std::vector<uint32_t> split;
        require(HdUEWeldCorners(triangles,10,[&](size_t a,size_t b){return (a<3)==(b<3);},split).size()==12 && split[0]!=split[3]);
        VtVec3iArray fan; for (int i=0;i<1000;++i) fan.push_back(GfVec3i(0,i+1,i+2)); // long chains on point 0 while vertices grow
        const auto distinct=HdUEWeldCorners(fan,1002,[](size_t a,size_t b){return a==b;},split);
        require(distinct.size()==fan.size()*3);
        for (size_t corner=0;corner<split.size();++corner) require(split[corner]==corner && distinct[corner]==corner);
        bool invalidIndex=false;
        try { HdUEWeldCorners(triangles,5,[](size_t,size_t){return true;},split); } catch(const std::invalid_argument&) { invalidIndex=true; }
        require(invalidIndex);
        HdUEMeshData mesh; mesh.id="/Mesh"; mesh.scenePath="/Source/Mesh"; mesh.material="/Looks/Default";
        mesh.indices=indices; mesh.sourceCorners=welded;
        for (const uint32_t corner:welded) {
            const int vertex=triangles[corner/3][corner%3];
            mesh.points.emplace_back(float(vertex),1,2); mesh.normals.emplace_back(0,1,0);
            mesh.uv.emplace_back(float(vertex),.25f);
            mesh.masks["wetness"].push_back(vertex/10.f);
        }
        auto parts=HdUEPartitionSubsets(mesh,topology,params);
        require(parts.size()==2 && parts[0].points.size()==6 && parts[1].points.size()==4 && parts[0].indices.size()==6 && parts[1].indices.size()==6);
        for (const auto& part:parts) for (const uint32_t index:part.indices) require(index<part.points.size() && part.sourceCorners.size()==part.points.size());
        require(parts[0].material==mesh.material && parts[1].material=="/Looks/Red");
        require(parts[1].id=="/Mesh/Quad" && parts[1].scenePath==mesh.scenePath);
        for (const auto& part:parts) for(size_t i=0;i<part.points.size();++i)
            require(part.points[i][0]==part.uv[i][0] && part.normals[i]==GfVec3f(0,1,0) && std::abs(part.masks.at("wetness")[i]-part.points[i][0]/10.f)<1e-6f);
        subset.indices={0,1,2}; topology.SetGeomSubsets({subset});
        parts=HdUEPartitionSubsets(mesh,topology,params);
        require(parts.size()==1 && parts[0].id=="/Mesh/Quad" && parts[0].points.size()==10 && parts[0].indices.size()==12);
        topology.SetGeomSubsets({}); parts=HdUEPartitionSubsets(mesh,topology,params);
        require(parts.size()==1 && parts[0].id=="/Mesh");
        // Missing faces are skipped; a face listed twice, or by two subsets, keeps its first subset.
        for (auto invalid: {VtIntArray{-1},VtIntArray{3}}) {
            subset.indices=invalid; topology.SetGeomSubsets({subset});
            parts=HdUEPartitionSubsets(mesh,topology,params);
            require(parts.size()==1 && parts[0].id=="/Mesh");
        }
        subset.indices={0,0}; topology.SetGeomSubsets({subset});
        require(HdUEPartitionSubsets(mesh,topology,params).size()==2);
        subset.indices={0}; auto overlap=subset; overlap.id=SdfPath("/Mesh/Overlap");
        topology.SetGeomSubsets({subset,overlap});
        parts=HdUEPartitionSubsets(mesh,topology,params);
        require(parts.size()==2 && parts[1].id=="/Mesh/Quad");
        mesh.sceneAsset="/Game/Native.Native";
        require(HdUEPartitionSubsets(mesh,topology,params).size()==1);
        const auto smooth=HdUEComputeSmoothNormals(VtVec3fArray{GfVec3f(0),GfVec3f(2,0,0),GfVec3f(0,2,0),GfVec3f(0,0,1)},
            VtVec3iArray{GfVec3i(0,1,2),GfVec3i(0,3,1)});
        require(smooth.size()==4 && smooth[2]==GfVec3f(0,0,1));
        require((smooth[0]-GfVec3f(0,2,4).GetNormalized()).GetLength()<1e-6f);
        const auto degenerate=HdUEComputeSmoothNormals(VtVec3fArray{GfVec3f(0)},VtVec3iArray{GfVec3i(0)});
        require(degenerate[0]==GfVec3f(0));
        std::cout<<"PASS quad triangulation, fallback faces, attributes, removal, invalid subsets and native slots\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
