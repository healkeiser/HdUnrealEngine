#include "IdSwitch.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialFunctionInterface.h"

namespace {
// UE's Substrate standard_surface functions (what the Standard Surface masters and Interchange's MaterialX import call,
// InterchangeMaterialXPipeline.cpp): the inputs that zero every lobe, the ones set to 1 (coverage, emission weight).
const TCHAR* const StandardSurfaceZero[] = {TEXT("base"),           TEXT("base_color"),   TEXT("specular"),
                                            TEXT("specular_color"), TEXT("coat"),         TEXT("sheen"),
                                            TEXT("subsurface"),     TEXT("transmission"), TEXT("thin_film_thickness")};
const TCHAR* const StandardSurfaceOne[] = {TEXT("opacity"), TEXT("emission")};
bool IsFunction(const FString& Path, std::initializer_list<const TCHAR*> Names) {
    for (const TCHAR* Name : Names)
        if (Path == FString::Printf(TEXT("/Engine/Functions/Substrate/%s.%s"), Name, Name)) return true;
    return false;
}

// Each change is In = lerp(In, Value, HdUEIdPass): with the switch at 0 (every beauty frame) lerp returns In exactly,
// and the Substrate tree keeps its BSDFs, operators' parameter blending and features, so the beauty does not change.
// With the switch at 1 every lobe's albedo, F0 and fuzz are 0 (Substrate scales F90 and the energy terms by F0:
// Substrate.ush SubstrateBakeF90ToReflectivity, F0RGBToMicroOcclusion), coverage is 1 and the emission is the ID, as
// M_HdUEId draws it. The Preview Surface masters select a separate slab instead (HdUEPrepareCommandlet.cpp), which
// parameter blending would collapse layered graphs into.
struct FIdSwitch {
    UMaterial* Material;
    bool Apply = false;  // false: only check that every node of the graph can be switched
    UMaterialExpression* Switch = nullptr;
    UMaterialExpression* Emission = nullptr;

    template <class T>
    T* Add() {
        auto* Expression = NewObject<T>(Material);
        Material->GetExpressionCollection().AddExpression(Expression);
        return Expression;
    }
    UMaterialExpression* IdValue() {
        if (!Emission)
            Emission = IdEmission(Material, [&](auto* E) {
                Material->GetExpressionCollection().AddExpression(E);
                return E;
            });
        return Emission;
    }
    // A graph switched before (the Substrate Builder and wet materials duplicate a prepared MaterialX material).
    bool Switched(const FExpressionInput& In) const {
        const UMaterialExpression* Traced = In.GetTracedInput().Expression;
        if (const auto* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Traced))
            return Lerp->Alpha.GetTracedInput().Expression == Switch;
        if (const auto* Minus = Cast<UMaterialExpressionOneMinus>(Traced))
            return Minus->Input.GetTracedInput().Expression == Switch;
        return false;
    }
    UMaterialExpression* Constant(const FVector4f& Value, int32 Components) {
        if (Components == 1) {
            auto* E = Add<UMaterialExpressionConstant>();
            E->R = Value.X;
            return E;
        }
        if (Components == 2) {
            auto* E = Add<UMaterialExpressionConstant2Vector>();
            E->R = Value.X;
            E->G = Value.Y;
            return E;
        }
        if (Components == 3) {
            auto* E = Add<UMaterialExpressionConstant3Vector>();
            E->Constant = FLinearColor(Value.X, Value.Y, Value.Z);
            return E;
        }
        auto* E = Add<UMaterialExpressionConstant4Vector>();
        E->Constant = FLinearColor(Value.X, Value.Y, Value.Z, Value.W);
        return E;
    }
    static bool Reads(const FVector4f& Default, int32 Components, float Value, bool Id) {
        bool Same = !Id;
        for (int32 C = 0; C < Components; ++C) Same &= Default[C] == Value;
        return Same;
    }
    // In = lerp(In, Id ? the ID emission : Value, HdUEIdPass); an unconnected In reads Default. An unconnected In
    // that already reads Value stays unconnected (a connection can turn a Substrate feature on: Fuzz, SSS).
    void Blend(FExpressionInput& In, const FVector4f& Default, int32 Components, float Value, bool Id = false) {
        if (!Apply || Switched(In) || (!In.IsConnected() && Reads(Default, Components, Value, Id))) return;
        auto* Lerp = Add<UMaterialExpressionLinearInterpolate>();
        if (In.IsConnected())
            Lerp->A = In;
        else
            Lerp->A.Connect(0, Constant(Default, Components));
        if (Id)
            Lerp->B.Connect(0, IdValue());
        else
            Lerp->ConstB = Value;
        Lerp->Alpha.Connect(0, Switch);
        In.Connect(0, Lerp);
    }
    bool Function(UMaterialExpressionMaterialFunctionCall* Call) {
        if (!Call->MaterialFunction) return false;
        if (!IsFunction(Call->MaterialFunction->GetPathName(),
                        {TEXT("Substrate-StandardSurface-Opaque"), TEXT("Substrate-StandardSurface-Translucent")}))
            return false;
        const auto Find = [&](const TCHAR* Name) -> FFunctionExpressionInput* {
            for (auto& Input : Call->FunctionInputs)
                if (Input.ExpressionInput && Input.ExpressionInput->InputName == Name) return &Input;
            return nullptr;
        };
        const auto Set = [&](FFunctionExpressionInput& Input, float Value, bool Id) {
            const UMaterialExpressionFunctionInput* Declaration = Input.ExpressionInput;
            const int32 Components = Declaration->InputType == FunctionInput_Scalar    ? 1
                                     : Declaration->InputType == FunctionInput_Vector2 ? 2
                                     : Declaration->InputType == FunctionInput_Vector3 ? 3
                                     : Declaration->InputType == FunctionInput_Vector4 ? 4
                                                                                       : 0;
            if (!Components) return false;
            // An unconnected input reads its default (bUsePreviewValueAsDefault): the expression on its Preview pin,
            // else PreviewValue (UMaterialFunction::LinkIntoCaller, UMaterialExpressionFunctionInput::Compile). Only a
            // constant can be reproduced in the caller.
            FVector4f Default = Declaration->PreviewValue;
            if (!Input.Input.IsConnected()) {
                if (!Declaration->bUsePreviewValueAsDefault) return false;
                if (const UMaterialExpression* Preview = Declaration->Preview.GetTracedInput().Expression) {
                    if (const auto* C = Cast<UMaterialExpressionConstant>(Preview))
                        Default = FVector4f(C->R, C->R, C->R, C->R);
                    else if (const auto* C2 = Cast<UMaterialExpressionConstant2Vector>(Preview))
                        Default = FVector4f(C2->R, C2->G, 0, 0);
                    else if (const auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Preview))
                        Default = FVector4f(C3->Constant.R, C3->Constant.G, C3->Constant.B, 0);
                    else if (const auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Preview))
                        Default = FVector4f(C4->Constant);
                    else
                        return false;
                }
            }
            Blend(Input.Input, Default, Components, Value, Id);
            return true;
        };
        auto* EmissionColor = Find(TEXT("emission_color"));
        if (!EmissionColor) return false;
        for (const TCHAR* Name : StandardSurfaceZero)
            if (auto* Input = Find(Name); Input && !Set(*Input, 0.f, false)) return false;
        for (const TCHAR* Name : StandardSurfaceOne)
            if (auto* Input = Find(Name); Input && !Set(*Input, 1.f, false)) return false;
        return Set(*EmissionColor, 0.f, true);
    }
    bool Visit(FExpressionInput& In) {
        UMaterialExpression* Expression = In.GetTracedInput().Expression;
        if (auto* Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(Expression)) {
            const float DefaultF0 =
                .08f * .5f;  // UMaterialExpressionSubstrateSlabBSDF::Compile: DielectricSpecularToF0(.5)
            Blend(Slab->DiffuseAlbedo, FVector4f(.18f, .18f, .18f, 0), 3, 0.f);
            Blend(Slab->F0, FVector4f(DefaultF0, DefaultF0, DefaultF0, 0), 3, 0.f);
            Blend(Slab->FuzzAmount, FVector4f(0), 1, 0.f);
            // A simple volume's transmittance (glass) would show what is behind: a zero mean free path absorbs all.
            Blend(Slab->SSSMFP, FVector4f(0), 3, 0.f);
            Blend(Slab->EmissiveColor, FVector4f(0), 3, 0.f, true);
            return true;
        }
        if (auto* Unlit = Cast<UMaterialExpressionSubstrateUnlitBSDF>(Expression)) {
            if (IsTranslucentBlendMode(Material->BlendMode))
                Blend(Unlit->TransmittanceColor, FVector4f(1, 1, 1, 0), 3, 0.f);
            Blend(Unlit->EmissiveColor, FVector4f(0), 3, 0.f, true);
            return true;
        }
        if (auto* Weight = Cast<UMaterialExpressionSubstrateWeight>(Expression)) {
            Blend(Weight->Weight, FVector4f(1), 1, 1.f);  // full coverage
            return Visit(Weight->A);
        }
        if (auto* Mix = Cast<UMaterialExpressionSubstrateHorizontalMixing>(Expression)) {
            if (Switch && Mix->Mix.GetTracedInput().Expression == Switch) return true;  // an ID slab mixed in (below)
            Blend(Mix->Mix, FVector4f(.5f), 1, 0.f);                                    // the background alone
            return Visit(Mix->Background);
        }
        if (auto* Layer = Cast<UMaterialExpressionSubstrateVerticalLayering>(Expression)) {
            // The top layer at zero coverage leaves the base unattenuated. A Weight operator holds no BSDF, so the
            // material keeps its footprint.
            const auto* Top = Cast<UMaterialExpressionSubstrateWeight>(Layer->Top.GetTracedInput().Expression);
            if (Apply && Layer->Top.IsConnected() && !(Top && Switched(Top->Weight))) {
                auto* Hide = Add<UMaterialExpressionSubstrateWeight>();
                Hide->A = Layer->Top;
                auto* Visible = Add<UMaterialExpressionOneMinus>();
                Visible->Input.Connect(0, Switch);
                Hide->Weight.Connect(0, Visible);
                Layer->Top.Connect(0, Hide);
            }
            return Visit(Layer->Base);
        }
        if (auto* Select = Cast<UMaterialExpressionSubstrateSelect>(Expression))
            return Visit(Select->A) && Visit(Select->B);
        if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression)) {
            if (Call->MaterialFunction &&
                IsFunction(Call->MaterialFunction->GetPathName(),
                           {TEXT("MF_Substrate_OpenPBR_Opaque"), TEXT("MF_Substrate_OpenPBR_Translucent")})) {
                // UE's OpenPBR scales its base slab's emission by more than its inputs (0.87 with every lobe off), so
                // the ID slab is mixed in instead: Mix 0 leaves the surface alone, 1 shows only the ID slab.
                if (Apply) {
                    auto* Zero = Add<UMaterialExpressionConstant>();
                    auto* Slab = Add<UMaterialExpressionSubstrateSlabBSDF>();
                    Slab->DiffuseAlbedo.Connect(0, Zero);
                    Slab->F0.Connect(0, Zero);
                    Slab->EmissiveColor.Connect(0, IdValue());
                    auto* Mix = Add<UMaterialExpressionSubstrateHorizontalMixing>();
                    Mix->Background = In;
                    Mix->Foreground.Connect(0, Slab);
                    Mix->Mix.Connect(0, Switch);
                    In.Connect(0, Mix);
                }
                return true;
            }
            return Function(Call);
        }
        // Add would sum two IDs; hair, eye, water, legacy conversions and unknown functions have no known switch.
        return false;
    }
};
}  // namespace

bool HdUEAddIdSwitch(UMaterial* Material) {
    auto& Front = Material->GetEditorOnlyData()->FrontMaterial;
    if (!Front.IsConnected()) return false;
    FIdSwitch Switch{Material};
    for (UMaterialExpression* Expression : Material->GetExpressions())
        if (const auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression);
            Scalar && Scalar->ParameterName == TEXT("HdUEIdPass"))
            Switch.Switch = Expression;
    if (!Switch.Visit(Front)) return false;
    if (!Switch.Switch) {
        auto* Parameter = Switch.Add<UMaterialExpressionScalarParameter>();
        Parameter->ParameterName = TEXT("HdUEIdPass");
        Parameter->DefaultValue = 0.f;
        Parameter->bUseCustomPrimitiveData = true;
        Parameter->PrimitiveDataIndex = 5;
        Switch.Switch = Parameter;
    }
    Switch.Apply = true;
    return Switch.Visit(Front);
}
