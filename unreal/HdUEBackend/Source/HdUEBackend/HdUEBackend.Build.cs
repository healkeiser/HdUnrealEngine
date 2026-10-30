using UnrealBuildTool;
using System.IO;
public class HdUEBackend : ModuleRules
{
    public HdUEBackend(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicIncludePaths.Add(Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../../include")));
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "RHI", "RenderCore", "Renderer", "Projects", "GeometryFramework", "GeometryCore", "MeshConversion", "MeshDescription", "StaticMeshDescription", "ImageWrapper", "ImageCore", "AssetRegistry", "IESFile", "NaniteBuilder", "ImageWriteQueue" });
        // Block compression of runtime textures (ispc_texcomp.dll, delay-loaded and staged with the runtime).
        AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelISPCTexComp", "MikkTSpace");
        if (Target.Type != TargetType.Editor)
        {
            // Pinned monolithic source target; private VT access is isolated in VirtualTextureResidency.cpp.
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("Renderer"), "Private"));
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("Renderer"), "Internal"));
            PrivateDependencyModuleNames.Add("UELibrary");
            PrivateDefinitions.Add("HDUE_BRIDGE_EXPORTS=1");
            // Shared frame textures and fences (SharedFrames.cpp, hdue/bridge.h).
            PrivateDependencyModuleNames.Add("D3D12RHI");
            AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
            // Lumen mesh cards: MeshCards.cpp compiles MeshUtilities' card builder (the module itself links the editor).
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("MeshUtilities"), "Private"));
            PrivateIncludePathModuleNames.AddRange(new[] { "MeshUtilities", "MeshMergeUtilities", "MeshReductionInterface", "RawMesh", "MeshUtilitiesCommon" });
            AddEngineThirdPartyPrivateStaticDependencies(Target, "Embree");
        }
    }
}

