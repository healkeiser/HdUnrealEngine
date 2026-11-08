#include "HdUEImportMaterialXCommandlet.h"
#include "WetMaterials.h"
#include "IdSwitch.h"
#include "HairUsage.h"
#include "HdUEMaterialXAssetsPipeline.h"
#include "InterchangeManager.h"
#include "InterchangeSourceData.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeMaterialXPipeline.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "EditorFramework/AssetImportData.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "UDIMUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/EngineVersion.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace {
int32 CVar(const TCHAR* Name) {
    const IConsoleVariable* Value = IConsoleManager::Get().FindConsoleVariable(Name);
    return Value ? Value->GetInt() : -1;
}
bool SaveImported(UObject* Object) {
    UPackage* Package = Object->GetOutermost();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    const FString File =
        FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    return UPackage::SavePackage(Package, Object, *File, Args);
}
FString FileHash(const FString& File) {
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *File)) return TEXT("");
    FSHAHash Hash;
    FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
    return Hash.ToString().ToLower();
}
// A change to these corrections (or the ID switch) changes what a graph imports to: bump IMPORT_VERSION in
// scripts/usd_materialx_graph.py with it, so live materials prepared before are prepared again.
int32 CorrectNormalScale(UMaterial* Material) {
    // Interchange uses FlattenNormal's lerp-to-(0,0,1). MaterialX instead
    // scales XY and normalizes, keeping the decoded Z component unchanged.
    const auto Original = Material->GetExpressions();
    TArray<UMaterialExpression*> Expressions;
    for (UMaterialExpression* Expression : Original) Expressions.Add(Expression);
    int32 Count = 0;
    for (auto* Expression : Expressions) {
        auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
        if (!Call || !Call->MaterialFunction ||
            !Call->MaterialFunction->GetPathName().EndsWith(TEXT("/FlattenNormal.FlattenNormal")))
            continue;
        FExpressionInput Normal, Flatness;
        for (const auto& Input : Call->FunctionInputs) {
            if (Input.ExpressionInput && Input.ExpressionInput->InputName == TEXT("Normal")) Normal = Input.Input;
            if (Input.ExpressionInput && Input.ExpressionInput->InputName == TEXT("Flatness")) Flatness = Input.Input;
        }
        if (!Normal.Expression || !Flatness.Expression) continue;
        auto* XY = NewObject<UMaterialExpressionComponentMask>(Material);
        XY->Input = Normal;
        XY->R = XY->G = true;
        XY->B = XY->A = false;
        auto* Z = NewObject<UMaterialExpressionComponentMask>(Material);
        Z->Input = Normal;
        Z->R = Z->G = Z->A = false;
        Z->B = true;
        auto* Scale = NewObject<UMaterialExpressionOneMinus>(Material);
        Scale->Input = Flatness;
        auto* Multiply = NewObject<UMaterialExpressionMultiply>(Material);
        Multiply->A.Connect(0, XY);
        Multiply->B.Connect(0, Scale);
        auto* Append = NewObject<UMaterialExpressionAppendVector>(Material);
        Append->A.Connect(0, Multiply);
        Append->B.Connect(0, Z);
        auto* Normalize = NewObject<UMaterialExpressionNormalize>(Material);
        Normalize->VectorInput.Connect(0, Append);
        for (auto* Consumer : Expressions)
            for (FExpressionInputIterator It{Consumer}; It; ++It)
                if (It->Expression == Call) It->Connect(0, Normalize);
        for (UMaterialExpression* Added : TArray<UMaterialExpression*>{XY, Z, Scale, Multiply, Append, Normalize})
            Material->GetExpressionCollection().AddExpression(Added);
        Material->GetExpressionCollection().RemoveExpression(Call);
        ++Count;
    }
    return Count;
}
int32 CorrectPlace2DRotation(UMaterial* Material) {
    // MaterialX's place2d rotates by `rotate` degrees. UE 5.8.4's MX_Place2D function scales it by 1/360 (turns) and
    // feeds UE's Rotator, which reads radians (UMaterialExpressionRotator::Compile: cos(Time * Speed), Speed 1): 30
    // degrees turn by 30/360 rad, 4.8 degrees. Scaling the input by 2 pi makes it rotate by the authored degrees.
    const auto Original = Material->GetExpressions();
    TArray<UMaterialExpression*> Expressions;
    for (UMaterialExpression* Expression : Original) Expressions.Add(Expression);
    int32 Count = 0;
    for (auto* Expression : Expressions) {
        auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
        if (!Call || !Call->MaterialFunction ||
            !Call->MaterialFunction->GetPathName().EndsWith(TEXT("/MX_Place2D.MX_Place2D")))
            continue;
        for (auto& Input : Call->FunctionInputs) {
            if (!Input.ExpressionInput || Input.ExpressionInput->InputName != TEXT("rotate") || !Input.Input.Expression)
                continue;
            auto* Turns = NewObject<UMaterialExpressionMultiply>(Material);
            Turns->A = Input.Input;
            Turns->ConstB = 2 * UE_PI;
            Input.Input.Connect(0, Turns);
            Material->GetExpressionCollection().AddExpression(Turns);
            ++Count;
        }
    }
    return Count;
}
int32 ReplaceFlakeMarkers(UMaterial* Material) {
    // UE 5.8.4's MaterialX importer has no flake2d/flake3d (MaterialX 1.39.5). hdUE's export turns a flake with plain
    // inputs into marker constants, which Interchange imports as parameters named
    // <node>_hdueflake_<mode>_<size>_<roughness>_<coverage>_<output>_value (scripts/materialx_lowering.py
    // lower_flakes); this puts MaterialX's own flake code in their place (Shaders/MaterialXFlake.ush). Its inputs are
    // what UE's importer reads for the nodedef's defaults: texcoord UV0 (TextureCoordinate 0), position Pobject (Local
    // Position in metres, ConnectPositionInputToOutput), normal and tangent VertexNormalWS and VertexTangentWS, the
    // bitangent their cross product (ConnectBitangentInputToOutput). The flake normal is in world space, as
    // MaterialX's.
    static const TCHAR* Outputs[] = {TEXT("flakenormal"), TEXT("id"), TEXT("rand"), TEXT("presence")};
    TMap<FString, TMap<FString, UMaterialExpressionParameter*>> Flakes;
    for (UMaterialExpression* Expression : Material->GetExpressions()) {
        auto* Parameter = Cast<UMaterialExpressionParameter>(Expression);
        if (!Parameter) continue;
        const FString Name = Parameter->ParameterName.ToString();
        int32 Output = INDEX_NONE;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Outputs); ++Index)
            if (Name.EndsWith(FString::Printf(TEXT("_%s_value"), Outputs[Index]))) Output = Index;
        if (Output == INDEX_NONE || !Name.Contains(TEXT("_hdueflake_"))) continue;
        const FString Suffix = FString::Printf(TEXT("_%s_value"), Outputs[Output]);
        Flakes.FindOrAdd(Name.LeftChop(Suffix.Len())).Add(Outputs[Output], Parameter);
    }
    TArray<UMaterialExpression*> Expressions;
    for (UMaterialExpression* Expression : Material->GetExpressions()) Expressions.Add(Expression);
    int32 Count = 0;
    for (const auto& [Key, Markers] : Flakes) {
        FString Code;
        Key.Split(TEXT("_hdueflake_"), nullptr, &Code, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        TArray<FString> Fields;
        Code.ParseIntoArray(Fields, TEXT("_"));
        if (Fields.Num() != 4) {
            UE_LOG(LogTemp, Error, TEXT("Unreadable flake marker: %s"), *Key);
            continue;
        }
        const auto Value = [&](int32 Index) { return FCString::Atod(*Fields[Index]) / 1e6; };
        UMaterialExpression* Position;
        if (Fields[0] == TEXT("0")) {
            auto* UV = NewObject<UMaterialExpressionTextureCoordinate>(Material);
            auto* Zero = NewObject<UMaterialExpressionConstant>(Material);
            auto* Append = NewObject<UMaterialExpressionAppendVector>(Material);
            Append->A.Connect(0, UV);
            Append->B.Connect(0, Zero);
            for (UMaterialExpression* Added : TArray<UMaterialExpression*>{UV, Zero, Append})
                Material->GetExpressionCollection().AddExpression(Added);
            Position = Append;
        } else {
            auto* Local = NewObject<UMaterialExpressionLocalPosition>(Material);
            auto* Metres = NewObject<UMaterialExpressionMultiply>(Material);
            Metres->A.Connect(0, Local);
            Metres->ConstB = 0.01f;
            Material->GetExpressionCollection().AddExpression(Local);
            Material->GetExpressionCollection().AddExpression(Metres);
            Position = Metres;
        }
        auto* Custom = NewObject<UMaterialExpressionCustom>(Material);
        Custom->Description = TEXT("MaterialX flake");
        Custom->IncludeFilePaths.Add(TEXT("/HdUE/MaterialXFlake.ush"));
        Custom->Inputs.SetNum(1);
        Custom->Inputs[0].InputName = TEXT("Position");
        Custom->Inputs[0].Input.Connect(0, Position);
        Custom->OutputType = CMOT_Float3;
        for (const TCHAR* Name : {TEXT("Id"), TEXT("Rand"), TEXT("Presence")}) {
            FCustomOutput& Output = Custom->AdditionalOutputs.AddDefaulted_GetRef();
            Output.OutputName = Name;
            Output.OutputType = CMOT_Float1;
        }
        Custom->Code = FString::Printf(
            TEXT("const float3 N = Parameters.TangentToWorld[2];\n"
                 "const float3 T = Parameters.TangentToWorld[0];\n"
                 "return HdUEMaterialXFlake(%.9g, %.9g, %.9g, Position, N, T, cross(N, T), Id, Rand, Presence);"),
            Value(1), Value(2), Value(3));
        Custom->RebuildOutputs();
        Material->GetExpressionCollection().AddExpression(Custom);
        // Consumers of a marker read the Custom output of the same name (output 0 the normal, then Id, Rand,
        // Presence); a vector marker's single-channel outputs (1-3: R, G, B) through a mask of the normal.
        const auto Reconnect = [&](FExpressionInput* Input) {
            if (!Input || !Input->Expression) return;
            for (int32 Output = 0; Output < UE_ARRAY_COUNT(Outputs); ++Output) {
                UMaterialExpressionParameter* const* Marker = Markers.Find(Outputs[Output]);
                if (!Marker || Input->Expression != *Marker) continue;
                if (Output == 0 && Input->OutputIndex > 0) {
                    auto* Channel = NewObject<UMaterialExpressionComponentMask>(Material);
                    Channel->Input.Connect(0, Custom);
                    Channel->R = Input->OutputIndex == 1;
                    Channel->G = Input->OutputIndex == 2;
                    Channel->B = Input->OutputIndex == 3;
                    Channel->A = false;
                    Material->GetExpressionCollection().AddExpression(Channel);
                    Input->Connect(0, Channel);
                } else
                    Input->Connect(Output, Custom);
            }
        };
        for (UMaterialExpression* Consumer : Expressions)
            for (FExpressionInputIterator It{Consumer}; It; ++It) Reconnect(It.Input);
        for (int32 Property = 0; Property < MP_MAX; ++Property)
            Reconnect(Material->GetExpressionInputForProperty(EMaterialProperty(Property)));
        for (const auto& [Output, Marker] : Markers) Material->GetExpressionCollection().RemoveExpression(Marker);
        ++Count;
    }
    return Count;
}
}  // namespace
UHdUEImportMaterialXCommandlet::UHdUEImportMaterialXCommandlet() {
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}
int32 UHdUEImportMaterialXCommandlet::Main(const FString& Params) {
    FString Source, Destination, Report;
    if (!FParse::Value(*Params, TEXT("Source="), Source) ||
        !FParse::Value(*Params, TEXT("Destination="), Destination) ||
        !FParse::Value(*Params, TEXT("Report="), Report) || !Destination.StartsWith(TEXT("/Game/"))) {
        UE_LOG(LogTemp, Error, TEXT("Required: -Source=file.mtlx -Destination=/Game/... -Report=file.json"));
        return 1;
    }
    Source = FPaths::ConvertRelativePathToFull(Source);
    const FString SourceHash = FileHash(Source);
    if (!FPaths::FileExists(Source) || CVar(TEXT("r.Substrate")) != 1 ||
        CVar(TEXT("r.Substrate.ProjectGBufferFormat")) != 1) {
        UE_LOG(LogTemp, Error, TEXT("MaterialX import requires an existing source and Substrate Adaptive GBuffer."));
        return 2;
    }
    auto Pipeline = TStrongObjectPtr<UHdUEMaterialXAssetsPipeline>(NewObject<UHdUEMaterialXAssetsPipeline>());
    Pipeline->MaterialPipeline->bImportMaterials = true;
    Pipeline->MaterialPipeline->MaterialImport = EInterchangeMaterialImportOption::ImportAsMaterials;
    Pipeline->MaterialPipeline->bReuseExistingMaterials = false;
    auto SourceData = TStrongObjectPtr<UInterchangeSourceData>(UInterchangeManager::CreateSourceData(Source));
    FImportAssetParameters Import;
    Import.bIsAutomated = true;
    Import.bReplaceExisting = true;
    Import.OverridePipelines.Add(FSoftObjectPath(Pipeline.Get()));
    // MaterialX resolves its function-call factory nodes after the generic
    // material pipeline has created them, matching UE's normal import stack.
    auto MaterialX = TStrongObjectPtr<UInterchangeMaterialXPipeline>(NewObject<UInterchangeMaterialXPipeline>());
    Import.OverridePipelines.Add(FSoftObjectPath(MaterialX.Get()));
    TArray<UObject*> Objects;
    if (!UInterchangeManager::GetInterchangeManager().ImportAsset(Destination, SourceData.Get(), Import, Objects)) {
        UE_LOG(LogTemp, Error, TEXT("MaterialX import failed: %s"), *Source);
        return 3;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    TArray<TSharedPtr<FJsonValue>> Assets;
    auto Dependencies = MakeShared<FJsonObject>();
    TSet<UTexture*> PreparedTextures;
    int32 TextureSourceErrors = 0;
    int32 TextureSaveErrors = 0;
    const auto PrepareTexture = [&](UTexture* Texture) {
        if (!Texture || PreparedTextures.Contains(Texture)) return;
        PreparedTextures.Add(Texture);
        if (Texture->Source.GetNumBlocks() > 1 && Texture->AssetImportData) {
            // UE's MaterialX blocked payload reports its last alternate path
            // as source 0. Rebuild provenance from the actual block locations.
            const auto OriginalFiles = Texture->AssetImportData->ExtractFilenames();
            auto Tiles = OriginalFiles.IsEmpty()
                             ? TMap<int32, FString>()
                             : UE::TextureUtilitiesCommon::GetUDIMBlocksFromSourceFile(
                                   OriginalFiles[0], UE::TextureUtilitiesCommon::DefaultUdimRegexPattern);
            TArray<FAssetImportInfo::FSourceFile> Sources;
            for (int32 I = 0; I < Texture->Source.GetNumBlocks(); ++I) {
                FTextureSourceBlock Block;
                Texture->Source.GetBlock(I, Block);
                const FString* File = Tiles.Find(1001 + Block.BlockX + 10 * Block.BlockY);
                if (!File) {
                    ++TextureSourceErrors;
                    break;
                }
                Sources.Emplace(*File, IFileManager::Get().GetTimeStamp(**File), FMD5Hash::HashFile(**File));
            }
            if (Sources.Num() == Texture->Source.GetNumBlocks())
                Texture->AssetImportData->SetSourceFiles(MoveTemp(Sources));
        }
        if (Texture->CompressionSettings == TC_Normalmap) {
            // USD/MaterialX use +Y tangent normals; UE's flipped V basis
            // requires the DirectX green convention for the same direction.
            Texture->SRGB = false;
            Texture->bFlipGreenChannel = true;
            Texture->PostEditChange();
        }
        if (Texture->AssetImportData)
            for (const FString& File : Texture->AssetImportData->ExtractFilenames())
                Dependencies->SetStringField(File, FileHash(File));
        if (!SaveImported(Texture)) ++TextureSaveErrors;
    };
    Dependencies->SetStringField(Source, SourceHash);
    int32 MaterialCount = 0;
    for (UObject* Object : Objects) {
        if (!IsValid(Object)) continue;
        auto Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Object->GetName());
        Entry->SetStringField(TEXT("asset"), Object->GetPathName());
        Entry->SetStringField(TEXT("class"), Object->GetClass()->GetName());
        // PrepareTexture records the texture's source files and saves it.
        if (auto* Texture = Cast<UTexture>(Object)) PrepareTexture(Texture);
        if (auto* Interface = Cast<UMaterialInterface>(Object)) {
            UMaterial* Material = Interface->GetMaterial();
            if (!Material || !Material->HasSubstrateFrontMaterialConnected()) {
                UE_LOG(LogTemp, Error, TEXT("MaterialX did not generate native Substrate: %s"), *Object->GetPathName());
                return 4;
            }
            ++MaterialCount;
            Material->SetUsageByFlag(MATUSAGE_Nanite, true);
            // Cooked runtimes cannot compile usage permutations; Hydra instancers draw through ISMs.
            Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
            HdUESetHairUsage(Material);  // chiang_hair_bsdf: grooms bound to it draw with it
            Entry->SetNumberField(TEXT("normal_scale_corrections"), CorrectNormalScale(Material));
            Entry->SetNumberField(TEXT("place2d_rotation_corrections"), CorrectPlace2DRotation(Material));
            Entry->SetNumberField(TEXT("flakes"), ReplaceFlakeMarkers(Material));
            // ID passes switch the material to its ID emission instead of swapping it (false: it keeps the swap).
            Entry->SetBoolField(TEXT("id_switch"), HdUEAddIdSwitch(Material));
            Material->PostEditChange();
            Entry->SetBoolField(TEXT("native_substrate"), true);
            Entry->SetNumberField(TEXT("blend_mode"), int32(Interface->GetBlendMode()));
            TArray<FMaterialParameterInfo> Info;
            TArray<FGuid> Guids;
            TArray<TSharedPtr<FJsonValue>> Parameters;
            Interface->GetAllScalarParameterInfo(Info, Guids);
            for (const auto& P : Info) {
                float Value = 0;
                Interface->GetScalarParameterValue(P, Value);
                auto Field = MakeShared<FJsonObject>();
                Field->SetStringField(TEXT("name"), P.Name.ToString());
                Field->SetStringField(TEXT("type"), TEXT("scalar"));
                Field->SetNumberField(TEXT("default"), Value);
                Parameters.Add(MakeShared<FJsonValueObject>(Field));
            }
            Info.Empty();
            Guids.Empty();
            Interface->GetAllVectorParameterInfo(Info, Guids);
            for (const auto& P : Info) {
                FLinearColor Value;
                Interface->GetVectorParameterValue(P, Value);
                auto Field = MakeShared<FJsonObject>();
                Field->SetStringField(TEXT("name"), P.Name.ToString());
                Field->SetStringField(TEXT("type"), TEXT("vector"));
                Field->SetArrayField(TEXT("default"),
                                     {MakeShared<FJsonValueNumber>(Value.R), MakeShared<FJsonValueNumber>(Value.G),
                                      MakeShared<FJsonValueNumber>(Value.B), MakeShared<FJsonValueNumber>(Value.A)});
                Parameters.Add(MakeShared<FJsonValueObject>(Field));
            }
            Info.Empty();
            Guids.Empty();
            Interface->GetAllTextureParameterInfo(Info, Guids);
            for (const auto& P : Info) {
                UTexture* Value = nullptr;
                Interface->GetTextureParameterValue(P, Value);
                // Interchange may reuse a previously imported texture without
                // returning it in Objects: preparing it records its sources too.
                PrepareTexture(Value);
                auto Field = MakeShared<FJsonObject>();
                Field->SetStringField(TEXT("name"), P.Name.ToString());
                Field->SetStringField(TEXT("type"), TEXT("texture"));
                Field->SetStringField(TEXT("default"), Value ? Value->GetPathName() : TEXT(""));
                if (Value) {
                    Field->SetBoolField(TEXT("virtual_texture"), Value->VirtualTextureStreaming);
                    Field->SetBoolField(TEXT("normal_map"), Value->CompressionSettings == TC_Normalmap);
                    Field->SetBoolField(TEXT("srgb"), Value->SRGB);
                    Field->SetBoolField(TEXT("flip_green"), Value->bFlipGreenChannel);
                    TArray<TSharedPtr<FJsonValue>> Files, Blocks;
                    if (Value->AssetImportData)
                        for (const FString& File : Value->AssetImportData->ExtractFilenames())
                            Files.Add(MakeShared<FJsonValueString>(File));
                    for (int32 I = 0; I < Value->Source.GetNumBlocks(); ++I) {
                        FTextureSourceBlock Block;
                        Value->Source.GetBlock(I, Block);
                        auto Data = MakeShared<FJsonObject>();
                        Data->SetNumberField(TEXT("x"), Block.BlockX);
                        Data->SetNumberField(TEXT("y"), Block.BlockY);
                        Data->SetNumberField(TEXT("width"), Block.SizeX);
                        Data->SetNumberField(TEXT("height"), Block.SizeY);
                        Blocks.Add(MakeShared<FJsonValueObject>(Data));
                    }
                    Field->SetArrayField(TEXT("source_files"), Files);
                    Field->SetArrayField(TEXT("source_blocks"), Blocks);
                }
                Parameters.Add(MakeShared<FJsonValueObject>(Field));
            }
            Entry->SetArrayField(TEXT("parameters"), Parameters);
            if (FParse::Param(*Params, TEXT("WetProfile"))) {
                auto Effects = MakeShared<FJsonObject>();
                Effects->SetObjectField(TEXT("wet"), HdUEPrepareWetMaterials(Material));
                Entry->SetObjectField(TEXT("surface_effects"), Effects);
            }
        }
        if (!Cast<UTexture>(Object) && !SaveImported(Object)) return 5;
        Assets.Add(MakeShared<FJsonValueObject>(Entry));
    }
    if (!MaterialCount) {
        UE_LOG(LogTemp, Error, TEXT("MaterialX import generated no materials."));
        return 6;
    }
    if (TextureSaveErrors) {
        UE_LOG(LogTemp, Error, TEXT("%d imported textures could not be saved."), TextureSaveErrors);
        return 5;
    }
    if (TextureSourceErrors) {
        UE_LOG(LogTemp, Error, TEXT("UDIM source files do not match imported texture blocks."));
        return 8;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), Source);
    Result->SetStringField(TEXT("destination"), Destination);
    Result->SetStringField(TEXT("source_sha1"), SourceHash);
    Result->SetObjectField(TEXT("dependencies_sha1"), Dependencies);
    Result->SetNumberField(TEXT("substrate"), CVar(TEXT("r.Substrate")));
    Result->SetNumberField(TEXT("gbuffer_format"), CVar(TEXT("r.Substrate.ProjectGBufferFormat")));
    auto ShaderConfig = MakeShared<FJsonObject>();
    ShaderConfig->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    ShaderConfig->SetNumberField(TEXT("closures"), CVar(TEXT("r.Substrate.ProjectClosuresPerPixel")));
    ShaderConfig->SetNumberField(TEXT("bytes"), CVar(TEXT("r.Substrate.BytesPerPixel")));
    ShaderConfig->SetStringField(TEXT("target"), TEXT("Windows_SM6"));
    Result->SetObjectField(TEXT("shader_config"), ShaderConfig);
    Result->SetNumberField(TEXT("materials"), MaterialCount);
    Result->SetArrayField(TEXT("assets"), Assets);
    FString Json;
    auto Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Result, Writer);
    if (!FFileHelper::SaveStringToFile(Json, *Report, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 7;
    UE_LOG(LogTemp, Display, TEXT("hdUE imported %d native Substrate MaterialX materials."), MaterialCount);
    return 0;
}
