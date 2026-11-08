#pragma once
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVectorParameter.h"

// The ID passes' emission (HDUE_AOV_PRIM_ID): custom primitive data 0..2, set per component to the encoded Hydra
// prim ID; with custom primitive data 4 set (instance pass), the instance's Hydra instanceId from per-instance custom
// data 3 in the same encoding (exact in the float16 readback: both halves stay below 2048).
template <class FAdd>
UMaterialExpression* IdEmission(UMaterial* Material, const FAdd& Add) {
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
    return Select;
}

// The Substrate graph compiler's version; scripts/substrate_graph.py COMPILER_VERSION must match.
// 3: ID switch.
constexpr int32 HdUESubstrateCompilerVersion = 4;

// Gives a finished Substrate material the HdUEIdPass switch (custom primitive data 5) the backend sets for ID passes
// (Scene.cpp SetIdMode), so the mesh keeps its material instead of swapping to M_HdUEId. Call it before
// PostEditChange. Returns false, changing nothing, for a graph it cannot switch exactly; that material keeps the swap.
bool HdUEAddIdSwitch(UMaterial* Material);
