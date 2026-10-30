#include "HdUEPrepareCommandlet.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionPathTracingQualitySwitch.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionTextureSampleParameterVolume.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialFunction.h"
#include "Engine/VolumeTexture.h"
#include "Engine/Texture2D.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "StaticMeshCompiler.h"
#include "Misc/Paths.h"
#include "EngineUtils.h"

namespace {
bool SaveAsset(UObject* Asset) {
    UPackage* Package = Asset->GetOutermost();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString File =
        FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    FAssetRegistryModule::AssetCreated(Asset);
    return UPackage::SavePackage(Package, Asset, *File, Args);
}
UMaterial* CreatePBR(const TCHAR* Name, FLinearColor Color, float Metal, float Roughness, bool Translucent = false) {
    const FString Path = FString(TEXT("/Game/HdUE/")) + Name;
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".") + Name), nullptr, LOAD_NoWarn);
    if (Material && Material->HasSubstrateFrontMaterialConnected() &&
        (!Translucent || Material->GetBlendMode() == BLEND_TranslucentGreyTransmittance))
        return Material;
    if (!Material) Material = NewObject<UMaterial>(Package, Name, RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    auto* Base = NewObject<UMaterialExpressionVectorParameter>(Material);
    Base->ParameterName = TEXT("BaseColor");
    Base->DefaultValue = Color;
    auto* Metallic = NewObject<UMaterialExpressionScalarParameter>(Material);
    Metallic->ParameterName = TEXT("Metallic");
    Metallic->DefaultValue = Metal;
    auto* Rough = NewObject<UMaterialExpressionScalarParameter>(Material);
    Rough->ParameterName = TEXT("Roughness");
    Rough->DefaultValue = Roughness;
    auto* Texture = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
    Texture->ParameterName = TEXT("BaseColorTexture");
    Texture->Texture =
        LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
    auto* Multiply = NewObject<UMaterialExpressionMultiply>(Material);
    Multiply->A.Connect(0, Base);
    Multiply->B.Connect(0, Texture);
    auto* Conversion = NewObject<UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0>(Material);
    Conversion->BaseColor.Connect(0, Multiply);
    Conversion->Metallic.Connect(0, Metallic);
    auto* Slab = NewObject<UMaterialExpressionSubstrateSlabBSDF>(Material);
    Slab->DiffuseAlbedo.Connect(0, Conversion);
    Slab->F0.Connect(1, Conversion);
    Slab->Roughness.Connect(0, Rough);
    const TArray<UMaterialExpression*> Expressions = {Base, Metallic, Rough, Texture, Multiply, Conversion, Slab};
    for (UMaterialExpression* Expression : Expressions) Material->GetExpressionCollection().AddExpression(Expression);
    auto* Data = Material->GetEditorOnlyData();
    Data->BaseColor.Expression = nullptr;
    Data->Metallic.Expression = nullptr;
    Data->Roughness.Expression = nullptr;
    Data->FrontMaterial.Connect(0, Slab);
    if (Translucent) {
        // Opacity is coverage, with no fabricated glass IOR or absorption.
        auto* Opacity = NewObject<UMaterialExpressionScalarParameter>(Material);
        Opacity->ParameterName = TEXT("Opacity");
        Opacity->DefaultValue = 1.f;
        auto* Weight = NewObject<UMaterialExpressionSubstrateWeight>(Material);
        Weight->A.Connect(0, Slab);
        Weight->Weight.Connect(0, Opacity);
        Material->GetExpressionCollection().AddExpression(Opacity);
        Material->GetExpressionCollection().AddExpression(Weight);
        Data->FrontMaterial.Connect(0, Weight);
        Material->BlendMode = BLEND_TranslucentGreyTransmittance;
        Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
    }
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
UTexture2D* CreateLinearWhite() {
    // Default for data-texture samplers, which must not be sRGB.
    if (auto* Existing =
            LoadObject<UTexture2D>(nullptr, TEXT("/Game/HdUE/T_LinearWhite.T_LinearWhite"), nullptr, LOAD_NoWarn))
        return Existing;
    UPackage* Package = CreatePackage(TEXT("/Game/HdUE/T_LinearWhite"));
    auto* Texture = NewObject<UTexture2D>(Package, TEXT("T_LinearWhite"), RF_Public | RF_Standalone);
    const uint8 White[4] = {255, 255, 255, 255};
    Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, White);
    Texture->SRGB = false;
    Texture->CompressionSettings = TC_VectorDisplacementmap;
    Texture->MipGenSettings = TMGS_NoMipmaps;
    Texture->PostEditChange();
    return SaveAsset(Texture) ? Texture : nullptr;
}
// Surface master. Every input is texel * <Slot>Scale + <Slot>Bias (scalar inputs then dot
// <Slot>Mask), so an untextured input is Scale 0, Bias constant. Preview Surface masters shade
// one Substrate slab; Standard Surface masters (Layered: MaterialX subsurface and sheen) feed the
// same inputs to UE's Substrate standard_surface function, whose slabs and layers cost more per pixel.
UMaterial* CreateSurface(const TCHAR* Name, EBlendMode Blend, UTexture* LinearWhite, bool Layered = false,
                         bool Thin = false) {
    const FString Path = FString(TEXT("/Game/HdUE/")) + Name;
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".") + Name), nullptr, LOAD_NoWarn);
    const auto HasParameter = [](UMaterial* Existing, const TCHAR* Parameter) {
        for (const auto& Expression : Existing->GetExpressionCollection().Expressions)
            if (const auto* Named = Cast<UMaterialExpressionScalarParameter>(Expression);
                Named && Named->ParameterName == Parameter)
                return true;
        return false;
    };
    if (Material && Material->GetBlendMode() == Blend && Material->bUsedWithInstancedStaticMeshes &&
        Material->TwoSided &&
        HasParameter(Material, Layered ? TEXT("StandardSurfaceVersion3") : TEXT("PreviewSurfaceVersion5")) &&
        Material->bUsedWithNanite)
        for (const auto& Expression : Material->GetExpressionCollection().Expressions)
            if (Expression->IsA<UMaterialExpressionPerInstanceCustomData3Vector>()) return Material;
    if (!Material) Material = NewObject<UMaterial>(Package, Name, RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    TArray<UMaterialExpression*> Expressions;
    const auto Add = [&](auto* Expression) {
        Expressions.Add(Expression);
        return Expression;
    };
    const auto Vector = [&](const FString& Parameter, FLinearColor Value) {
        auto* Expression = Add(NewObject<UMaterialExpressionVectorParameter>(Material));
        Expression->ParameterName = *Parameter;
        Expression->DefaultValue = Value;
        return Expression;
    };
    // The delegate's UV affine: u' = dot((u,v,1), UVRow0.rgb), v' = dot((u,v,1), UVRow1.rgb), of each of the mesh's
    // UV sets (Nanite keeps 4).
    UMaterialExpression* Sets[4];
    for (int32 Set = 0; Set < 4; ++Set) {
        auto* Coordinate = Add(NewObject<UMaterialExpressionTextureCoordinate>(Material));
        Coordinate->CoordinateIndex = Set;
        auto* One = Add(NewObject<UMaterialExpressionConstant>(Material));
        One->R = 1.f;
        auto* Homogeneous = Add(NewObject<UMaterialExpressionAppendVector>(Material));
        Homogeneous->A.Connect(0, Coordinate);
        Homogeneous->B.Connect(0, One);
        UMaterialExpressionDotProduct* Rows[2];
        for (int32 Row = 0; Row < 2; ++Row) {
            Rows[Row] = Add(NewObject<UMaterialExpressionDotProduct>(Material));
            Rows[Row]->A.Connect(0, Homogeneous);
            Rows[Row]->B.Connect(
                0, Vector(FString::Printf(TEXT("UVRow%d"), Row), Row ? FLinearColor(0, 1, 0) : FLinearColor(1, 0, 0)));
        }
        auto* UV = Add(NewObject<UMaterialExpressionAppendVector>(Material));
        UV->A.Connect(0, Rows[0]);
        UV->B.Connect(0, Rows[1]);
        Sets[Set] = UV;
    }
    const auto Slot = [&](const FString& Slot, FLinearColor Default, bool Color, bool Scalar) -> UMaterialExpression* {
        // The UV set the slot's texture reads: sum of the sets weighted by <Slot>UVSet (one-hot; a mesh with fewer
        // sets repeats its last).
        UMaterialExpression* UV = nullptr;
        auto* Weights = Vector(Slot + TEXT("UVSet"), FLinearColor(1, 0, 0, 0));
        for (int32 Set = 0; Set < 4; ++Set) {
            auto* Weight = Add(NewObject<UMaterialExpressionComponentMask>(Material));
            Weight->Input.Connect(5, Weights);  // RGBA
            Weight->R = Set == 0;
            Weight->G = Set == 1;
            Weight->B = Set == 2;
            Weight->A = Set == 3;
            auto* Term = Add(NewObject<UMaterialExpressionMultiply>(Material));
            Term->A.Connect(0, Sets[Set]);
            Term->B.Connect(0, Weight);
            if (UV) {
                auto* Sum = Add(NewObject<UMaterialExpressionAdd>(Material));
                Sum->A.Connect(0, UV);
                Sum->B.Connect(0, Term);
                UV = Sum;
            } else
                UV = Term;
        }
        // Wrap mode black: textures are clamped, and <Slot>Black (per axis, 0 or 1) zeroes texels outside [0,1], so
        // the input reads its bias there: texel * Scale * (1 - saturate(dot(|floor(uv)|, Black.rg))).
        auto* Tile = Add(NewObject<UMaterialExpressionFloor>(Material));
        Tile->Input.Connect(0, UV);
        auto* Outside = Add(NewObject<UMaterialExpressionAbs>(Material));
        Outside->Input.Connect(0, Tile);
        auto* Sample = Add(NewObject<UMaterialExpressionTextureSampleParameter2D>(Material));
        Sample->ParameterName = *(Slot + TEXT("Texture"));
        Sample->Coordinates.Connect(0, UV);
        Sample->SamplerType = Color ? SAMPLERTYPE_Color : SAMPLERTYPE_LinearColor;
        Sample->Texture =
            Color ? LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"))
                  : LinearWhite;
        auto* BlackAxes = Add(NewObject<UMaterialExpressionComponentMask>(Material));
        BlackAxes->Input.Connect(0, Vector(Slot + TEXT("Black"), FLinearColor(0, 0, 0, 0)));
        BlackAxes->R = BlackAxes->G = 1;
        auto* Outer = Add(NewObject<UMaterialExpressionDotProduct>(Material));
        Outer->A.Connect(0, Outside);
        Outer->B.Connect(0, BlackAxes);
        auto* Clamped = Add(NewObject<UMaterialExpressionSaturate>(Material));
        Clamped->Input.Connect(0, Outer);
        auto* Inside = Add(NewObject<UMaterialExpressionOneMinus>(Material));
        Inside->Input.Connect(0, Clamped);
        auto* Scale = Add(NewObject<UMaterialExpressionMultiply>(Material));
        Scale->A.Connect(5, Vector(Slot + TEXT("Scale"), FLinearColor(0, 0, 0, 0)));
        Scale->B.Connect(0, Inside);
        auto* Scaled = Add(NewObject<UMaterialExpressionMultiply>(Material));
        Scaled->A.Connect(5, Sample);
        Scaled->B.Connect(0, Scale);
        auto* Biased = Add(NewObject<UMaterialExpressionAdd>(Material));
        Biased->A.Connect(0, Scaled);
        Biased->B.Connect(5, Vector(Slot + TEXT("Bias"), Default));
        if (Scalar) {
            auto* Channel = Add(NewObject<UMaterialExpressionDotProduct>(Material));
            Channel->A.Connect(0, Biased);
            Channel->B.Connect(5, Vector(Slot + TEXT("Mask"), FLinearColor(1, 0, 0, 0)));
            return Channel;
        }
        auto* Rgb = Add(NewObject<UMaterialExpressionComponentMask>(Material));
        Rgb->Input.Connect(0, Biased);
        Rgb->R = Rgb->G = Rgb->B = 1;
        Rgb->A = 0;
        return Rgb;
    };
    auto* Surface = Slot(TEXT("BaseColor"), FLinearColor(.7f, .7f, .7f, 1), true, false);
    // displayColor primvars: vertex colors times ISM per-instance custom data, enabled by UseVertexColor.
    auto* VertexColor = Add(NewObject<UMaterialExpressionVertexColor>(Material));
    auto* InstanceColor = Add(NewObject<UMaterialExpressionPerInstanceCustomData3Vector>(Material));
    InstanceColor->DataIndex = 0;
    InstanceColor->ConstDefaultValue = FLinearColor(1, 1, 1, 1);
    auto* Primvar = Add(NewObject<UMaterialExpressionMultiply>(Material));
    Primvar->A.Connect(0, VertexColor);
    Primvar->B.Connect(0, InstanceColor);
    auto* UseVertexColor = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
    UseVertexColor->ParameterName = TEXT("UseVertexColor");
    UseVertexColor->DefaultValue = 0.f;
    auto* Tint = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
    Tint->ConstA = 1.f;
    Tint->B.Connect(0, Primvar);
    Tint->Alpha.Connect(0, UseVertexColor);
    auto* BaseColor = Add(NewObject<UMaterialExpressionMultiply>(Material));
    BaseColor->A.Connect(0, Surface);
    BaseColor->B.Connect(0, Tint);
    auto* Emissive = Slot(TEXT("Emissive"), FLinearColor(0, 0, 0, 0), true, false);
    auto* Metallic = Slot(TEXT("Metallic"), FLinearColor(0, 0, 0, 0), false, true);
    auto* Roughness = Slot(TEXT("Roughness"), FLinearColor(.5f, .5f, .5f, .5f), false, true);
    auto* Opacity = Slot(TEXT("Opacity"), FLinearColor(1, 1, 1, 1), false, true);
    auto* Normal = Slot(TEXT("Normal"), FLinearColor(0, 0, 1, 0), false, false);
    // UsdPreviewSurface displacement moves the mesh's vertices in the delegate (displacement.h): the path tracer's
    // ray-tracing geometry ignores World Position Offset and Nanite tessellation.
    // USD tangent-space normals are +Y up (OpenGL); UE's are -Y after the V flip.
    auto* FlipGreen = Add(NewObject<UMaterialExpressionConstant3Vector>(Material));
    FlipGreen->Constant = FLinearColor(1, -1, 1);
    auto* UENormal = Add(NewObject<UMaterialExpressionMultiply>(Material));
    UENormal->A.Connect(0, Normal);
    UENormal->B.Connect(0, FlipGreen);
    auto* UnitNormal = Add(NewObject<UMaterialExpressionNormalize>(Material));
    UnitNormal->VectorInput.Connect(0, UENormal);
    auto* Occlusion = Slot(TEXT("Occlusion"), FLinearColor(1, 1, 1, 1), false, true);
    auto* Clearcoat = Slot(TEXT("Clearcoat"), FLinearColor(0, 0, 0, 0), false, true);
    auto* ClearcoatRoughness = Slot(TEXT("ClearcoatRoughness"), FLinearColor(.01f, .01f, .01f, .01f), false, true);
    auto* SpecularColor = Slot(TEXT("SpecularColor"), FLinearColor(0, 0, 0, 1), true, false);
    const auto Scalar = [&](const TCHAR* Parameter, float Value) {
        auto* Expression = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
        Expression->ParameterName = Parameter;
        Expression->DefaultValue = Value;
        return Expression;
    };
    UMaterialExpression* Front = nullptr;
    auto* Data = Material->GetEditorOnlyData();
    Data->BaseColor.Expression = nullptr;
    Data->Metallic.Expression = nullptr;
    Data->Roughness.Expression = nullptr;
    if (Layered) {
        Scalar(TEXT("StandardSurfaceVersion3"),
               1.f);  // unconnected marker: bump the name when this graph changes, to regenerate
        auto* Call = Add(NewObject<UMaterialExpressionMaterialFunctionCall>(Material));
        Call->SetMaterialFunction(LoadObject<UMaterialFunction>(
            nullptr,
            TEXT("/Engine/Functions/Substrate/Substrate-StandardSurface-Opaque.Substrate-StandardSurface-Opaque")));
        const auto Input = [&](const TCHAR* Input, UMaterialExpression* Expression) {
            for (auto& Function : Call->FunctionInputs)
                if (Function.ExpressionInput && Function.ExpressionInput->InputName == Input) {
                    Function.Input.Connect(0, Expression);
                    return;
                }
            UE_LOG(LogTemp, Error, TEXT("Substrate standard_surface has no input %s"), Input);
        };
        const auto Constant = [&](float Value) {
            auto* Expression = Add(NewObject<UMaterialExpressionConstant>(Material));
            Expression->R = Value;
            return Expression;
        };
        // subsurface_color and sheen_color: a tint, times the base colour texture when they read its image.
        const auto Tinted = [&](const TCHAR* Parameter) {
            auto* Follow = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
            Follow->ConstA = 1.f;
            Follow->B.Connect(0, Surface);
            Follow->Alpha.Connect(0, Scalar(*(FString(Parameter) + TEXT("BaseTexture")), 0.f));
            auto* Color = Add(NewObject<UMaterialExpressionMultiply>(Material));
            Color->A.Connect(0, Vector(FString(Parameter) + TEXT("Color"), FLinearColor::White));
            Color->B.Connect(0, Follow);
            return Color;
        };
        // The delegate folds the base, specular and emission weights into their colours.
        Input(TEXT("base"), Constant(1.f));
        Input(TEXT("base_color"), BaseColor);
        Input(TEXT("metalness"), Metallic);
        Input(TEXT("specular"), Constant(1.f));
        Input(TEXT("specular_color"), SpecularColor);
        Input(TEXT("specular_roughness"), Roughness);
        Input(TEXT("specular_IOR"), Scalar(TEXT("SpecularIOR"), 1.5f));
        Input(TEXT("coat"), Clearcoat);
        Input(TEXT("coat_color"), Vector(TEXT("CoatColor"), FLinearColor::White));
        Input(TEXT("coat_roughness"), ClearcoatRoughness);
        Input(TEXT("sheen"), Scalar(TEXT("Sheen"), 0.f));
        Input(TEXT("sheen_color"), Tinted(TEXT("Sheen")));
        Input(TEXT("sheen_roughness"), Scalar(TEXT("SheenRoughness"), .3f));
        Input(TEXT("subsurface"), Scalar(TEXT("Subsurface"), 0.f));
        Input(TEXT("subsurface_color"), Tinted(TEXT("Subsurface")));
        Input(TEXT("subsurface_radius"), Vector(TEXT("SubsurfaceRadius"), FLinearColor::White));
        Input(TEXT("subsurface_scale"), Constant(1.f));
        Input(TEXT("emission"), Constant(1.f));
        Input(TEXT("emission_color"), Emissive);
        Input(TEXT("normal"), UnitNormal);
        auto* ThinWalled = Add(NewObject<UMaterialExpressionStaticBool>(Material));
        ThinWalled->Value = Thin;
        Input(TEXT("thin_walled"), ThinWalled);
        Front = Call;
    } else {
        // Metalness workflow, with ior as the dielectric specular level (the backend converts ior to it).
        auto* Conversion = Add(NewObject<UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0>(Material));
        Conversion->BaseColor.Connect(0, BaseColor);
        Conversion->Metallic.Connect(0, Metallic);
        Conversion->Specular.Connect(0, Scalar(TEXT("SpecularLevel"), .5f));
        // Specular workflow (useSpecularWorkflow): diffuseColor and specularColor are the albedo and F0 directly.
        auto* UseSpecular = Scalar(TEXT("UseSpecularWorkflow"), 0.f);
        Scalar(TEXT("PreviewSurfaceVersion5"),
               5.f);  // unconnected marker: bump the name when this graph changes, to regenerate
        auto* Albedo = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
        Albedo->A.Connect(0, Conversion);
        Albedo->B.Connect(0, BaseColor);
        Albedo->Alpha.Connect(0, UseSpecular);
        auto* F0 = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
        F0->A.Connect(1, Conversion);
        F0->B.Connect(0, SpecularColor);
        F0->Alpha.Connect(0, UseSpecular);
        auto* Slab = Add(NewObject<UMaterialExpressionSubstrateSlabBSDF>(Material));
        Slab->DiffuseAlbedo.Connect(0, Albedo);
        Slab->F0.Connect(0, F0);
        Slab->Roughness.Connect(0, Roughness);
        Slab->Normal.Connect(0, UnitNormal);
        Slab->EmissiveColor.Connect(0, Emissive);
        // Clear coat as UE converts its own: a second specular lobe on the slab (Substrate haziness), with
        // clearcoatRoughness and weighted by clearcoat. An approximation of a coat layer, without the coat's absorption
        // over the base.
        Slab->SecondRoughness.Connect(0, ClearcoatRoughness);
        Slab->SecondRoughnessWeight.Connect(0, Clearcoat);
        Front = Slab;
    }
    Data->FrontMaterial.Connect(0, Front);
    Data->AmbientOcclusion.Connect(0, Occlusion);
    Material->BlendMode = Blend;
    if (Blend == BLEND_Masked) {
        // Cutout at opacityThreshold, independent of UE's per-asset clip value.
        auto* Threshold = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
        Threshold->ParameterName = TEXT("OpacityThreshold");
        Threshold->DefaultValue = .5f;
        auto* Coverage = Add(NewObject<UMaterialExpressionSubtract>(Material));
        Coverage->A.Connect(0, Opacity);
        Coverage->B.Connect(0, Threshold);
        auto* Centered = Add(NewObject<UMaterialExpressionAdd>(Material));
        Centered->A.Connect(0, Coverage);
        Centered->ConstB = .5f;
        Data->OpacityMask.Connect(0, Centered);
        Material->OpacityMaskClipValue = .5f;
    } else if (Blend == BLEND_TranslucentGreyTransmittance) {
        auto* Weight = Add(NewObject<UMaterialExpressionSubstrateWeight>(Material));
        Weight->A.Connect(0, Front);
        Weight->Weight.Connect(0, Opacity);
        Data->FrontMaterial.Connect(0, Weight);
        Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
    }
    // Two-sided like Karma and most path tracers, which ignore doubleSided: production assets often leave it unset
    // on thin single-sided geometry that is meant to be seen from both sides.
    Material->TwoSided = true;
    // Cooked runtimes cannot compile usage permutations; Hydra instancers draw through ISMs.
    Material->bUsedWithInstancedStaticMeshes = true;
    Material->bUsedWithNanite = true;  // runtime-built Nanite meshes (Scene.cpp BuildNanite)
    for (UMaterialExpression* Expression : Expressions) Material->GetExpressionCollection().AddExpression(Expression);
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
// AOV post process: sum of SceneTexture lookups (Substrate first BSDF) weighted by
// Aov1..Aov6; the backend sets one weight to 1 to output that HDUE_AOV_* channel.
UMaterial* CreateAovMaterial() {
    const FString Path = TEXT("/Game/HdUE/PP_HdUEAov");
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, TEXT("/Game/HdUE/PP_HdUEAov.PP_HdUEAov"), nullptr, LOAD_NoWarn);
    if (Material && Material->MaterialDomain == MD_PostProcess) return Material;
    if (!Material) Material = NewObject<UMaterial>(Package, TEXT("PP_HdUEAov"), RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    TArray<UMaterialExpression*> Expressions;
    const auto Add = [&](auto* Expression) {
        Expressions.Add(Expression);
        return Expression;
    };
    // Order matches HDUE_AOV_ALBEDO..HDUE_AOV_OCCLUSION; scalar channels broadcast to RGB.
    const ESceneTextureId Ids[] = {PPI_BaseColor, PPI_WorldNormal, PPI_Roughness,
                                   PPI_Metallic,  PPI_Specular,    PPI_MaterialAO};
    UMaterialExpression* Sum = nullptr;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ids); ++Index) {
        auto* Lookup = Add(NewObject<UMaterialExpressionSceneTexture>(Material));
        Lookup->SceneTextureId = Ids[Index];
        auto* Channel = Add(NewObject<UMaterialExpressionComponentMask>(Material));
        Channel->Input.Connect(0, Lookup);
        Channel->R = 1;
        Channel->G = Channel->B = Index < 2;
        Channel->A = 0;
        auto* Weight = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
        Weight->ParameterName = *FString::Printf(TEXT("Aov%d"), Index + 1);
        Weight->DefaultValue = 0;
        auto* Term = Add(NewObject<UMaterialExpressionMultiply>(Material));
        Term->A.Connect(0, Channel);
        Term->B.Connect(0, Weight);
        if (Sum) {
            auto* Next = Add(NewObject<UMaterialExpressionAdd>(Material));
            Next->A.Connect(0, Sum);
            Next->B.Connect(0, Term);
            Sum = Next;
        } else
            Sum = Term;
    }
    // Scalar terms are float; the first term is RGB, so the sum is RGB.
    auto* Output = Add(NewObject<UMaterialExpressionSubstratePostProcess>(Material));
    Output->Color.Connect(0, Sum);
    Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Output);
    Material->MaterialDomain = MD_PostProcess;
    Material->BlendableLocation = BL_SceneColorBeforeDOF;
    for (UMaterialExpression* Expression : Expressions) Material->GetExpressionCollection().AddExpression(Expression);
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
// ID pass material: unlit emission of custom primitive data 0..2, set per component to the
// encoded Hydra prim ID (see HDUE_AOV_PRIM_ID). With custom primitive data 4 set (instance pass),
// it emits the instance's Hydra instanceId from per-instance custom data 3 in the same encoding.
UMaterial* CreateIdMaterial() {
    const FString Path = TEXT("/Game/HdUE/M_HdUEId");
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, TEXT("/Game/HdUE/M_HdUEId.M_HdUEId"), nullptr, LOAD_NoWarn);
    if (Material && Material->HasSubstrateFrontMaterialConnected() && Material->bUsedWithInstancedStaticMeshes)
        for (const auto& Expression : Material->GetExpressionCollection().Expressions)
            if (Expression->IsA<UMaterialExpressionPerInstanceCustomData>()) return Material;
    if (!Material) Material = NewObject<UMaterial>(Package, TEXT("M_HdUEId"), RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    const auto Add = [&](auto* Expression) {
        Material->GetExpressionCollection().AddExpression(Expression);
        return Expression;
    };
    auto* Id = Add(NewObject<UMaterialExpressionVectorParameter>(Material));
    Id->ParameterName = TEXT("HdUEId");
    Id->DefaultValue = FLinearColor(0, 0, 0, 0);
    Id->bUseCustomPrimitiveData = true;
    Id->PrimitiveDataIndex = 0;
    auto* InstanceMode = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
    InstanceMode->ParameterName = TEXT("HdUEInstanceMode");
    InstanceMode->DefaultValue = 0.f;
    InstanceMode->bUseCustomPrimitiveData = true;
    InstanceMode->PrimitiveDataIndex = 4;
    auto* Instance = Add(NewObject<UMaterialExpressionPerInstanceCustomData>(Material));
    Instance->DataIndex = 3;
    Instance->ConstDefaultValue = -1.f;
    auto* Scaled = Add(NewObject<UMaterialExpressionDivide>(Material));
    Scaled->A.Connect(0, Instance);
    Scaled->ConstB = 2048.f;
    auto* High = Add(NewObject<UMaterialExpressionFloor>(Material));
    High->Input.Connect(0, Scaled);
    auto* HighScaled = Add(NewObject<UMaterialExpressionMultiply>(Material));
    HighScaled->A.Connect(0, High);
    HighScaled->ConstB = 2048.f;
    auto* Low = Add(NewObject<UMaterialExpressionSubtract>(Material));
    Low->A.Connect(0, Instance);
    Low->B.Connect(0, HighScaled);
    auto* HighLow = Add(NewObject<UMaterialExpressionAppendVector>(Material));
    HighLow->A.Connect(0, High);
    HighLow->B.Connect(0, Low);
    auto* One = Add(NewObject<UMaterialExpressionConstant>(Material));
    One->R = 1.f;
    auto* Encoded = Add(NewObject<UMaterialExpressionAppendVector>(Material));
    Encoded->A.Connect(0, HighLow);
    Encoded->B.Connect(0, One);
    auto* Select = Add(NewObject<UMaterialExpressionLinearInterpolate>(Material));
    Select->A.Connect(0, Id);
    Select->B.Connect(0, Encoded);
    Select->Alpha.Connect(0, InstanceMode);
    auto* Unlit = Add(NewObject<UMaterialExpressionSubstrateUnlitBSDF>(Material));
    Unlit->EmissiveColor.Connect(0, Select);
    Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Unlit);
    Material->bUsedWithNanite = true;
    Material->bUsedWithInstancedStaticMeshes = true;
    Material->TwoSided = true;
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
// Heterogeneous volume material for dense density grids: UVW_j = dot((world, 1), WorldToUVW_j),
// rows the backend derives from the voxel transform; extinction = density * ExtinctionScale.
UMaterial* CreateVolumeMaterial() {
    UVolumeTexture* Empty = LoadObject<UVolumeTexture>(nullptr, TEXT("/Game/HdUE/T_HdUEVolumeEmpty.T_HdUEVolumeEmpty"),
                                                       nullptr, LOAD_NoWarn);
    if (!Empty) {
        UPackage* Package = CreatePackage(TEXT("/Game/HdUE/T_HdUEVolumeEmpty"));
        Empty = NewObject<UVolumeTexture>(Package, TEXT("T_HdUEVolumeEmpty"), RF_Public | RF_Standalone);
        const uint8 Black[4] = {0, 0, 0, 0};
        Empty->Source.Init(1, 1, 1, 1, TSF_BGRA8, Black);
        Empty->SRGB = false;
        Empty->CompressionSettings = TC_VectorDisplacementmap;
        Empty->MipGenSettings = TMGS_NoMipmaps;
        Empty->PostEditChange();
        if (!SaveAsset(Empty)) return nullptr;
    }
    const FString Path = TEXT("/Game/HdUE/M_HdUEVolume");
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, TEXT("/Game/HdUE/M_HdUEVolume.M_HdUEVolume"), nullptr, LOAD_NoWarn);
    if (Material && Material->MaterialDomain == MD_Volume && Material->bUsedWithHeterogeneousVolumes) {
        for (const auto& Expression : Material->GetExpressionCollection().Expressions)
            if (const auto* Row = Cast<UMaterialExpressionVectorParameter>(Expression);
                Row && Row->ParameterName == TEXT("WorldToUVW0"))
                return Material;
    }
    if (!Material) Material = NewObject<UMaterial>(Package, TEXT("M_HdUEVolume"), RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    TArray<UMaterialExpression*> Expressions;
    const auto Add = [&](auto* Expression) {
        Expressions.Add(Expression);
        return Expression;
    };
    auto* World = Add(NewObject<UMaterialExpressionWorldPosition>(Material));
    auto* One = Add(NewObject<UMaterialExpressionConstant>(Material));
    One->R = 1.f;
    auto* Homogeneous = Add(NewObject<UMaterialExpressionAppendVector>(Material));
    Homogeneous->A.Connect(0, World);
    Homogeneous->B.Connect(0, One);
    UMaterialExpressionDotProduct* Axes[3];
    for (int32 Axis = 0; Axis < 3; ++Axis) {
        auto* Row = Add(NewObject<UMaterialExpressionVectorParameter>(Material));
        Row->ParameterName = *FString::Printf(TEXT("WorldToUVW%d"), Axis);
        Row->DefaultValue = FLinearColor(0, 0, 0, 0);
        Axes[Axis] = Add(NewObject<UMaterialExpressionDotProduct>(Material));
        Axes[Axis]->A.Connect(0, Homogeneous);
        Axes[Axis]->B.Connect(5, Row);
    }
    auto* UV = Add(NewObject<UMaterialExpressionAppendVector>(Material));
    UV->A.Connect(0, Axes[0]);
    UV->B.Connect(0, Axes[1]);
    auto* UVW = Add(NewObject<UMaterialExpressionAppendVector>(Material));
    UVW->A.Connect(0, UV);
    UVW->B.Connect(0, Axes[2]);
    auto* Density = Add(NewObject<UMaterialExpressionTextureSampleParameterVolume>(Material));
    Density->ParameterName = TEXT("Density");
    Density->Texture = Empty;
    Density->SamplerType = SAMPLERTYPE_LinearColor;
    Density->Coordinates.Connect(0, UVW);
    auto* Scale = Add(NewObject<UMaterialExpressionScalarParameter>(Material));
    Scale->ParameterName = TEXT("ExtinctionScale");
    Scale->DefaultValue = 1.f;
    auto* Extinction = Add(NewObject<UMaterialExpressionMultiply>(Material));
    Extinction->A.Connect(1, Density);
    Extinction->B.Connect(0, Scale);
    auto* Albedo = Add(NewObject<UMaterialExpressionVectorParameter>(Material));
    Albedo->ParameterName = TEXT("Albedo");
    Albedo->DefaultValue = FLinearColor(.8f, .8f, .8f, 1);
    auto* Data = Material->GetEditorOnlyData();
    // Legacy volume inputs, as UE's SparseVolumeTextureMaterialFactory uses: albedo, extinction.
    Data->BaseColor.Connect(0, Albedo);
    Data->SubsurfaceColor.Connect(0, Extinction);
    Material->MaterialDomain = MD_Volume;
    Material->BlendMode = BLEND_Additive;
    Material->bUsedWithHeterogeneousVolumes = true;
    for (UMaterialExpression* Expression : Expressions) Material->GetExpressionCollection().AddExpression(Expression);
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
UMaterial* CreateGlass() {
    const FString Path = TEXT("/Game/HdUE/M_Glass");
    UPackage* Package = CreatePackage(*Path);
    auto* Material = LoadObject<UMaterial>(nullptr, TEXT("/Game/HdUE/M_Glass.M_Glass"), nullptr, LOAD_NoWarn);
    if (Material && Material->HasSubstrateFrontMaterialConnected()) {
        for (const auto& Expression : Material->GetExpressionCollection().Expressions)
            if (Cast<UMaterialExpressionPathTracingQualitySwitch>(Expression)) return Material;
    }
    if (!Material) Material = NewObject<UMaterial>(Package, TEXT("M_Glass"), RF_Public | RF_Standalone);
    Material->PreEditChange(nullptr);
    Material->GetExpressionCollection().Empty();
    const auto Scalar = [&](const TCHAR* Name, float Value) {
        auto* Expression = NewObject<UMaterialExpressionScalarParameter>(Material);
        Expression->ParameterName = Name;
        Expression->DefaultValue = Value;
        Material->GetExpressionCollection().AddExpression(Expression);
        return Expression;
    };
    auto* IOR = Scalar(TEXT("IOR"), 1.5f);
    auto* Roughness = Scalar(TEXT("Roughness"), .04f);
    auto* Distance = Scalar(TEXT("AttenuationDistanceCm"), 1.f);
    auto* Thickness = Scalar(TEXT("SurfaceThicknessCm"), .2f);
    auto* Coverage = Scalar(TEXT("Coverage"), 1.f);
    auto* Color = NewObject<UMaterialExpressionVectorParameter>(Material);
    Color->ParameterName = TEXT("AttenuationColor");
    Color->DefaultValue = FLinearColor(.95f, .98f, .95f);
    auto* MFP = NewObject<UMaterialExpressionSubstrateTransmittanceToMFP>(Material);
    MFP->TransmittanceColor.Connect(0, Color);
    MFP->Thickness.Connect(0, Distance);
    // UE 5.8.4's thickness stack reads the root's constant, not its expression.
    // Keep that slab at 0.01 cm, scaling MFP so the realtime optical depth is
    // SurfaceThicknessCm / physical MFP. PT must retain unscaled world-space MFP.
    constexpr float RealtimeSlabThicknessCm = .01f;
    auto* ThicknessScale = NewObject<UMaterialExpressionDivide>(Material);
    ThicknessScale->ConstA = RealtimeSlabThicknessCm;
    ThicknessScale->B.Connect(0, Thickness);
    auto* RealtimeMFP = NewObject<UMaterialExpressionMultiply>(Material);
    RealtimeMFP->A.Connect(0, MFP);
    RealtimeMFP->B.Connect(0, ThicknessScale);
    auto* SelectedMFP = NewObject<UMaterialExpressionPathTracingQualitySwitch>(Material);
    SelectedMFP->Normal.Connect(0, RealtimeMFP);
    SelectedMFP->PathTraced.Connect(0, MFP);
    // Fresnel F0 = ((n-1)/(n+1))^2; the same IOR drives ray bending.
    auto* Numerator = NewObject<UMaterialExpressionSubtract>(Material);
    Numerator->A.Connect(0, IOR);
    Numerator->ConstB = 1;
    auto* Denominator = NewObject<UMaterialExpressionAdd>(Material);
    Denominator->A.Connect(0, IOR);
    Denominator->ConstB = 1;
    auto* Ratio = NewObject<UMaterialExpressionDivide>(Material);
    Ratio->A.Connect(0, Numerator);
    Ratio->B.Connect(0, Denominator);
    auto* F0 = NewObject<UMaterialExpressionMultiply>(Material);
    F0->A.Connect(0, Ratio);
    F0->B.Connect(0, Ratio);
    auto* Black = NewObject<UMaterialExpressionConstant3Vector>(Material);
    Black->Constant = FLinearColor::Black;
    auto* Slab = NewObject<UMaterialExpressionSubstrateSlabBSDF>(Material);
    Slab->DiffuseAlbedo.Connect(0, Black);
    Slab->SubSurfaceType = MSS_SimpleVolume;
    Slab->F0.Connect(0, F0);
    Slab->Roughness.Connect(0, Roughness);
    Slab->SSSMFP.Connect(0, SelectedMFP);
    auto* Weight = NewObject<UMaterialExpressionSubstrateWeight>(Material);
    Weight->A.Connect(0, Slab);
    Weight->Weight.Connect(0, Coverage);
    for (auto* Expression : TArray<UMaterialExpression*>{Color, MFP, ThicknessScale, RealtimeMFP, SelectedMFP,
                                                         Numerator, Denominator, Ratio, F0, Black, Slab, Weight})
        Material->GetExpressionCollection().AddExpression(Expression);
    auto* Data = Material->GetEditorOnlyData();
    Data->FrontMaterial.Connect(0, Weight);
    Data->Refraction.Connect(0, IOR);
    Data->SurfaceThickness.Expression = nullptr;
    Data->SurfaceThickness.UseConstant = true;
    Data->SurfaceThickness.Constant = RealtimeSlabThicknessCm;
    Material->BlendMode = BLEND_TranslucentColoredTransmittance;
    Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
    Material->RefractionMethod = RM_IndexOfRefraction;
    Material->bIsThinSurface = false;
    Material->TwoSided = true;
    Material->PostEditChange();
    return SaveAsset(Material) ? Material : nullptr;
}
}  // namespace

UHdUEPrepareCommandlet::UHdUEPrepareCommandlet() {
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}
int32 UHdUEPrepareCommandlet::Main(const FString& Params) {
    UTexture2D* LinearWhite = CreateLinearWhite();
    if (!LinearWhite) return 3;
    UMaterial* Preview = CreateSurface(TEXT("M_PreviewSurface"), BLEND_Opaque, LinearWhite);
    UMaterial* Masked = CreateSurface(TEXT("M_PreviewSurfaceMasked"), BLEND_Masked, LinearWhite);
    UMaterial* Translucent =
        CreateSurface(TEXT("M_PreviewSurfaceTranslucent"), BLEND_TranslucentGreyTransmittance, LinearWhite);
    // Standard Surface masters, selected by the backend by blend mode and thin_walled (a static input of UE's
    // function).
    for (const bool Thin : {false, true})
        for (const EBlendMode Blend : {BLEND_Opaque, BLEND_Masked})
            if (!CreateSurface(
                    *FString::Printf(TEXT("M_StandardSurface%s%s"), Blend == BLEND_Masked ? TEXT("Masked") : TEXT(""),
                                     Thin ? TEXT("Thin") : TEXT("")),
                    Blend, LinearWhite, true, Thin))
                return 3;
    if (!Masked || !CreateAovMaterial() || !CreateIdMaterial() || !CreateVolumeMaterial()) return 3;
    UMaterial* Glass = CreateGlass();
    UMaterial* Native = CreatePBR(TEXT("M_Native"), FLinearColor(0.05f, 0.15f, 0.8f), 0.6f, 0.15f);
    UMaterial* Red = CreatePBR(TEXT("M_ReferenceRed"), FLinearColor(0.7f, 0.05f, 0.02f), 0.05f, 0.4f);
    UMaterial* Grey = CreatePBR(TEXT("M_ReferenceGrey"), FLinearColor(0.4f, 0.4f, 0.4f), 0.f, 0.7f);
    if (!Preview || !Translucent || !Glass || !Native || !Red || !Grey) return 3;
    for (auto* Material : {Preview, Native, Red, Grey}) {
        if (!Material->bUsedWithNanite) {
            Material->PreEditChange(nullptr);
            Material->bUsedWithNanite = true;
            Material->PostEditChange();
            if (!SaveAsset(Material)) return 3;
        }
    }
    UPackage* MeshPackage = CreatePackage(TEXT("/Game/HdUE/SM_Nanite"));
    auto* NativeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/HdUE/SM_Nanite.SM_Nanite"), nullptr, LOAD_NoWarn);
    if (!NativeMesh)
        NativeMesh = DuplicateObject<UStaticMesh>(
            LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")), MeshPackage, TEXT("SM_Nanite"));
    NativeMesh->SetFlags(RF_Public | RF_Standalone);
    NativeMesh->GetNaniteSettings().bEnabled = true;
    NativeMesh->bAllowCPUAccess = true;
    NativeMesh->NeverStream = true;
    NativeMesh->SetMaterial(0, Native);
    NativeMesh->Build(false);
    FStaticMeshCompilingManager::Get().FinishCompilation({NativeMesh});
    if (!SaveAsset(NativeMesh)) return 4;
    const FString MapFile =
        FPackageName::LongPackageNameToFilename(TEXT("/Game/HdUETest"), FPackageName::GetMapPackageExtension());
    UWorld* World = FPaths::FileExists(MapFile) ? UEditorLoadingAndSavingUtils::LoadMap(MapFile)
                                                : UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!World) return 1;
    TArray<AActor*> OldActors;
    for (TActorIterator<AActor> It(World); It; ++It)
        if (It->IsA<AStaticMeshActor>() || It->IsA<ADirectionalLight>()) OldActors.Add(*It);
    for (auto* Actor : OldActors) World->DestroyActor(Actor);
    auto* Cube = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, 50), FRotator::ZeroRotator);
    Cube->GetStaticMeshComponent()->SetStaticMesh(
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Cube->GetStaticMeshComponent()->SetMaterial(0, Red);
    auto* Ground = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -5), FRotator::ZeroRotator);
    Ground->GetStaticMeshComponent()->SetStaticMesh(
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Ground->GetStaticMeshComponent()->SetMaterial(0, Grey);
    Ground->SetActorScale3D(FVector(12, 12, 0.1));
    auto* Light = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-45, 60, 0));
    // ADirectionalLight's CDO has a -46 degree root rotation. Set the final
    // transform explicitly instead of composing a spawn rotation with it.
    Light->SetActorRotation(FRotator(-45, 60, 0));
    Light->GetLightComponent()->SetMobility(EComponentMobility::Movable);
    Light->GetLightComponent()->SetIntensity(5.0f);
    if (!UEditorLoadingAndSavingUtils::SaveMap(World, TEXT("/Game/HdUETest"))) return 2;
    UE_LOG(LogTemp, Display, TEXT("hdUE test map created at /Game/HdUETest"));
    return 0;
}
