#pragma once
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialFunctionInterface.h"

// Whether a material's graph draws hair: a Substrate Hair BSDF, directly or inside a material function (UE's MaterialX
// import translates chiang_hair_bsdf to one). Such a material gets UE's hair strands usage, which a cooked runtime
// cannot compile on demand, so a groom bound to it draws with it (HdueGroomDesc.material).
inline bool HdUEDrawsHair(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, int32 Depth = 0) {
    for (const auto& Expression : Expressions) {
        if (Expression && Expression->IsA<UMaterialExpressionSubstrateHairBSDF>()) return true;
        if (const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
            Call && Call->MaterialFunction && Depth < 8 &&
            HdUEDrawsHair(Call->MaterialFunction->GetExpressions(), Depth + 1))
            return true;
    }
    return false;
}
inline void HdUESetHairUsage(UMaterial* Material) {
    if (HdUEDrawsHair(Material->GetExpressions())) Material->SetUsageByFlag(MATUSAGE_HairStrands, true);
}
