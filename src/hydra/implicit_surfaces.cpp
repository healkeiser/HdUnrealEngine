// Implicit gprims (cube, sphere, cone, cylinder, capsule, plane) reach hdUE as meshes, as for hdEmbree.
#include "nurbs.h"
#include "pxr/base/gf/bbox3d.h"
#include "pxr/imaging/hd/collectionsSchema.h"
#include "pxr/imaging/hd/containerDataSourceEditor.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/materialSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/sceneIndexPrimView.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hdsi/utils.h"
#include "pxr/imaging/hd/extentSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hd/nurbsCurvesSchema.h"
#include "pxr/imaging/hd/nurbsPatchSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"
#include "pxr/imaging/hdsi/nurbsApproximatingSceneIndex.h"
#if __has_include("pxr/imaging/hdsi/particleFieldConversionSceneIndex.h")  // not in OpenUSD 26.03
#include "pxr/imaging/hdsi/particleFieldConversionSceneIndex.h"
#define HDUE_PARTICLE_FIELDS 1
#endif
#include "pxr/imaging/hdsi/renderPassPruneSceneIndex.h"
#include "pxr/imaging/hdsi/tetMeshConversionSceneIndex.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/usd/usdGeom/hermiteCurves.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/visibilityAPI.h"
#include "pxr/usdImaging/usdImaging/apiSchemaAdapter.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"
#include "pxr/usdImaging/usdImaging/hermiteCurvesAdapter.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/usdImaging/usdImaging/drawModeSceneIndex.h"
#include "pxr/usdImaging/usdImaging/geomModelSchema.h"
#include "pxr/usdImaging/usdImaging/usdPrimInfoSchema.h"
#include "pxr/base/tf/getenv.h"
#include <algorithm>
#include <iostream>
#include <optional>
#include <mutex>
#include <set>
PXR_NAMESPACE_OPEN_SCOPE
class HdUE_ImplicitSurfaceSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        const HdDataSourceBaseHandle toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        return HdsiImplicitSurfaceSceneIndex::New(
            input,
            HdRetainedContainerDataSource::New(HdPrimTypeTokens->sphere, toMesh, HdPrimTypeTokens->cube, toMesh,
                                               HdPrimTypeTokens->cone, toMesh, HdPrimTypeTokens->cylinder, toMesh,
                                               HdPrimTypeTokens->capsule, toMesh, HdPrimTypeTokens->plane, toMesh));
    }
};
// Light linking: the stage scene index carries lightLink/shadowLink as collections; this resolves them into the
// category ids lights report (GetLightParamValue lightLink/shadowLink) and the categories of each prim (GetCategories).
class HdUE_LightLinkingSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return HdsiLightLinkingSceneIndex::New(input, nullptr);
    }
};
// A scene index passing its input's prims through edit (which returns a replacement data source, or null to keep it).
class HdUE_EditingSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    using Edit = HdContainerDataSourceHandle (*)(const HdSceneIndexBase&, const SdfPath&, const HdSceneIndexPrim&);
    HdUE_EditingSceneIndex(const HdSceneIndexBaseRefPtr& input, Edit edit)
        : HdSingleInputFilteringSceneIndexBase(input), edit_(edit) {}
    HdSceneIndexPrim GetPrim(const SdfPath& path) const override {
        auto prim = _GetInputSceneIndex()->GetPrim(path);
        if (prim.dataSource)
            if (auto edited = edit_(*_GetInputSceneIndex(), path, prim)) prim.dataSource = edited;
        return prim;
    }
    SdfPathVector GetChildPrimPaths(const SdfPath& path) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    void _PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) override {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) override {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) override {
        _SendPrimsDirtied(entries);
    }

private:
    Edit edit_;
};
// NURBS curves and patches with their control points replaced by points sampled on them (order 2): what
// HdsiNurbsApproximatingSceneIndex then draws, a hull, is the curve or surface. ponytail: points only (other vertex
// primvars keep their control-point counts and are dropped by the rprims), sampled at the current time, recomputed
// per GetPrim; trim curves are ignored as by Hydra's approximation.
struct HdUENurbsSampling {
    static HdContainerDataSourceHandle edit(const HdSceneIndexBase&, const SdfPath&, const HdSceneIndexPrim& prim) {
        if (prim.primType == HdPrimTypeTokens->nurbsCurves) return curves(prim.dataSource);
        if (prim.primType == HdPrimTypeTokens->nurbsPatch) return patch(prim.dataSource);
        return nullptr;
    }
    template <class T, class H>
    static T get(const H& source, T fallback = T()) {
        return source ? T(source->GetTypedValue(0)) : fallback;
    }
    static VtValue primvar(const HdContainerDataSourceHandle& source, const TfToken& name) {
        const auto value = HdPrimvarsSchema::GetFromParent(source).GetPrimvar(name).GetPrimvarValue();
        return value ? value->GetValue(0) : VtValue();
    }
    static HdContainerDataSourceHandle withPoints(HdContainerDataSourceEditor& editor, const VtVec3fArray& points) {
        return editor
            .Set(HdPrimvarsSchema::GetPointsLocator().Append(HdPrimvarSchemaTokens->primvarValue),
                 HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points))
            .Finish();
    }
    static VtDoubleArray weights(const HdContainerDataSourceHandle& source) {
        const auto value = primvar(source, TfToken("pointWeights"));
        if (value.IsHolding<VtDoubleArray>()) return value.UncheckedGet<VtDoubleArray>();
        if (!value.IsHolding<VtFloatArray>()) return {};
        const auto& w = value.UncheckedGet<VtFloatArray>();
        return VtDoubleArray(w.begin(), w.end());
    }
    static HdContainerDataSourceHandle curves(const HdContainerDataSourceHandle& source) {
        const auto schema = HdNurbsCurvesSchema::GetFromParent(source);
        const auto counts = get<VtIntArray>(schema.GetCurveVertexCounts());
        const auto orders = get<VtIntArray>(schema.GetOrder());
        const auto knots = get<VtDoubleArray>(schema.GetKnots());
        const auto ranges = get<VtVec2dArray>(schema.GetRanges());
        const auto points = primvar(source, HdPrimvarsSchemaTokens->points);
        if (!points.IsHolding<VtVec3fArray>() || orders.size() != counts.size()) return source;
        const auto& cvs = points.UncheckedGet<VtVec3fArray>();
        const auto w = weights(source);
        VtVec3fArray sampled;
        VtIntArray sampledCounts, sampledOrders(counts.size(), 2);
        VtDoubleArray sampledKnots;
        size_t first = 0, knot = 0;
        for (size_t c = 0; c < counts.size(); ++c) {
            const int count = counts[c], order = orders[c];
            if (count < 0 || order < 0 || first + count > cvs.size() || knot + count + order > knots.size())
                return source;
            const auto range = c < ranges.size() ? ranges[c] : GfVec2d(0);
            const auto curve =
                HdUENurbsCurve(cvs.data() + first, w.size() == cvs.size() ? w.data() + first : nullptr, count, order,
                               knots.data() + knot, size_t(count + order), range[0], range[1]);
            if (curve.empty()) return source;
            sampled.insert(sampled.end(), curve.begin(), curve.end());
            sampledCounts.push_back(int(curve.size()));
            // Linear knots: 0, 0, 1, ..., n - 2, n - 2.
            sampledKnots.push_back(0);
            for (size_t i = 0; i + 1 < curve.size(); ++i) sampledKnots.push_back(double(i));
            sampledKnots.push_back(double(curve.size() - 2));
            first += count;
            knot += count + order;
        }
        const auto& at = HdNurbsCurvesSchema::GetDefaultLocator();
        HdContainerDataSourceEditor editor(source);
        editor
            .Set(at.Append(HdNurbsCurvesSchemaTokens->curveVertexCounts),
                 HdRetainedTypedSampledDataSource<VtIntArray>::New(sampledCounts))
            .Set(at.Append(HdNurbsCurvesSchemaTokens->order),
                 HdRetainedTypedSampledDataSource<VtIntArray>::New(sampledOrders))
            .Set(at.Append(HdNurbsCurvesSchemaTokens->knots),
                 HdRetainedTypedSampledDataSource<VtDoubleArray>::New(sampledKnots))
            .Set(at.Append(HdNurbsCurvesSchemaTokens->ranges), nullptr);
        return withPoints(editor, sampled);
    }
    static HdContainerDataSourceHandle patch(const HdContainerDataSourceHandle& source) {
        const auto schema = HdNurbsPatchSchema::GetFromParent(source);
        const auto points = primvar(source, HdPrimvarsSchemaTokens->points);
        if (!points.IsHolding<VtVec3fArray>()) return source;
        int uSamples = 0, vSamples = 0;
        const auto sampled = HdUENurbsPatch(
            points.UncheckedGet<VtVec3fArray>(), weights(source), get<int>(schema.GetUVertexCount()),
            get<int>(schema.GetVVertexCount()), get<int>(schema.GetUOrder()), get<int>(schema.GetVOrder()),
            get<VtDoubleArray>(schema.GetUKnots()), get<VtDoubleArray>(schema.GetVKnots()),
            get<GfVec2d>(schema.GetURange()), get<GfVec2d>(schema.GetVRange()), uSamples, vSamples);
        if (sampled.empty()) return source;
        const auto knots = [](int n) {
            VtDoubleArray out{0};
            for (int i = 0; i < n; ++i) out.push_back(i);
            out.push_back(n - 1);
            return out;
        };
        const auto integer = [](int i) { return HdRetainedTypedSampledDataSource<int>::New(i); };
        const auto open = HdRetainedTypedSampledDataSource<TfToken>::New(HdNurbsPatchSchemaTokens->open);
        const auto& at = HdNurbsPatchSchema::GetDefaultLocator();
        HdContainerDataSourceEditor editor(source);
        editor.Set(at.Append(HdNurbsPatchSchemaTokens->uVertexCount), integer(uSamples))
            .Set(at.Append(HdNurbsPatchSchemaTokens->vVertexCount), integer(vSamples))
            .Set(at.Append(HdNurbsPatchSchemaTokens->uOrder), integer(2))
            .Set(at.Append(HdNurbsPatchSchemaTokens->vOrder), integer(2))
            .Set(at.Append(HdNurbsPatchSchemaTokens->uKnots),
                 HdRetainedTypedSampledDataSource<VtDoubleArray>::New(knots(uSamples)))
            .Set(at.Append(HdNurbsPatchSchemaTokens->vKnots),
                 HdRetainedTypedSampledDataSource<VtDoubleArray>::New(knots(vSamples)))
            .Set(at.Append(HdNurbsPatchSchemaTokens->uForm), open)
            .Set(at.Append(HdNurbsPatchSchemaTokens->vForm), open)
            .Set(at.Append(HdNurbsPatchSchemaTokens->uRange), nullptr)
            .Set(at.Append(HdNurbsPatchSchemaTokens->vRange), nullptr);
        return withPoints(editor, sampled);
    }
};
// Prims hdUE has no rprim for, converted by Hydra's own scene indices: TetMesh to mesh, NURBS patches and curves to
// mesh and basisCurves, Gaussian-splat particle fields to points (with their UsdPreviewSurface approximation).
class HdUE_ConversionSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        HdSceneIndexBaseRefPtr result = HdsiTetMeshConversionSceneIndex::New(input);
        result = HdsiNurbsApproximatingSceneIndex::New(
            TfCreateRefPtr(new HdUE_EditingSceneIndex(result, HdUENurbsSampling::edit)));
#if defined(HDUE_PARTICLE_FIELDS)
        result = TfCreateRefPtr(new HdUE_EditingSceneIndex(
            HdsiParticleFieldConversionSceneIndex::New(result, nullptr, nullptr, nullptr), splats));
#endif
        return result;
    }
#if defined(HDUE_PARTICLE_FIELDS)
    // Hydra's particle-field points emit their spherical harmonics' DC term through a primvar reader on
    // emissiveColor (/__DefaultParticleFieldMaterial), which hdUE's materials do not translate: the points take it as
    // displayColor instead, unbound from that material, whose network is dropped (it is added to every scene).
    static HdContainerDataSourceHandle splats(const HdSceneIndexBase&, const SdfPath& path,
                                              const HdSceneIndexPrim& prim) {
        static const SdfPath material("/__DefaultParticleFieldMaterial");
        if (path == material)
            return HdContainerDataSourceEditor(prim.dataSource)
                .Set(HdMaterialSchema::GetDefaultLocator(), nullptr)
                .Finish();
        const auto type = UsdImagingUsdPrimInfoSchema::GetFromParent(prim.dataSource).GetTypeName();
        if (prim.primType != HdPrimTypeTokens->points || !type ||
            type->GetTypedValue(0) != "ParticleField3DGaussianSplat")
            return nullptr;
        static std::once_flag warned;
        std::call_once(warned, [&] {
            TF_WARN(
                "hdUE: ParticleField3DGaussianSplat renders as Hydra's points approximation (spheres coloured by "
                "the spherical harmonics' DC term), not as Gaussian splats (%s)",
                path.GetText());
        });
        HdContainerDataSourceEditor editor(prim.dataSource);
        if (const auto color = HdPrimvarsSchema::GetFromParent(prim.dataSource)
                                   .GetPrimvar(TfToken("radiance:sphericalHarmonicsCoefficients00")))
            editor.Set(HdPrimvarsSchema::GetDefaultLocator().Append(HdTokens->displayColor), color.GetContainer());
        if (HdsiUtilsGetBoundMaterial(prim) == material)
            editor.Set(HdMaterialBindingsSchema::GetDefaultLocator(), nullptr);
        return editor.Finish();
    }
#endif
};
// model:drawMode origin/bounds/cards, which UE's USD importer applies (USDGeomXformableTranslator.cpp,
// UUsdDrawModeComponent): UsdImaging's own draw-mode scene index, which husk does not add for a renderer.
class HdUE_DrawModeSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return UsdImagingDrawModeSceneIndex::New(TfCreateRefPtr(new HdUE_EditingSceneIndex(input, extent)), nullptr);
    }
    // The stand-ins span the model's extent, which UsdImaging's scene index takes from an authored extentsHint only;
    // UE (USDGeomXformableTranslator.cpp, UsdGeomBBoxCache) and UsdImaging's legacy draw-mode adapter compute it
    // otherwise: here, the union of the descendants' extents (or points).
    static HdContainerDataSourceHandle extent(const HdSceneIndexBase& input, const SdfPath& path,
                                              const HdSceneIndexPrim& prim) {
        const auto model = UsdImagingGeomModelSchema::GetFromParent(prim.dataSource);
        const auto apply = model.GetApplyDrawMode();
        const auto mode = model.GetDrawMode() ? model.GetDrawMode()->GetTypedValue(0) : TfToken();
        if (!apply || !apply->GetTypedValue(0) || mode.IsEmpty() || mode == "default" || mode == "inherited" ||
            HdExtentSchema::GetFromParent(prim.dataSource))
            return nullptr;
        const auto world = [](const HdContainerDataSourceHandle& source) {
            const auto matrix = HdXformSchema::GetFromParent(source).GetMatrix();
            return matrix ? matrix->GetTypedValue(0) : GfMatrix4d(1);
        };
        const auto toModel = world(prim.dataSource).GetInverse();
        GfRange3d box;
        SdfPathVector stack = input.GetChildPrimPaths(path);
        while (!stack.empty()) {
            const auto child = stack.back();
            stack.pop_back();
            const auto source = input.GetPrim(child).dataSource;
            // Its extent, else the bounds of its points.
            const auto extent = HdExtentSchema::GetFromParent(source);
            GfRange3d local;
            if (extent.GetMin() && extent.GetMax())
                local = GfRange3d(extent.GetMin()->GetTypedValue(0), extent.GetMax()->GetTypedValue(0));
            else if (const auto points = HdUENurbsSampling::primvar(source, HdPrimvarsSchemaTokens->points);
                     points.IsHolding<VtVec3fArray>())
                for (const auto& point : points.UncheckedGet<VtVec3fArray>()) local.UnionWith(GfVec3d(point));
            if (!local.IsEmpty()) box.UnionWith(GfBBox3d(local, world(source) * toModel).ComputeAlignedRange());
            for (const auto& grandchild : input.GetChildPrimPaths(child)) stack.push_back(grandchild);
        }
        if (box.IsEmpty()) return nullptr;
        const auto extent = HdExtentSchema::Builder()
                                .SetMin(HdRetainedTypedSampledDataSource<GfVec3d>::New(box.GetMin()))
                                .SetMax(HdRetainedTypedSampledDataSource<GfVec3d>::New(box.GetMax()))
                                .Build();
        return HdContainerDataSourceEditor(prim.dataSource).Set(HdExtentSchema::GetDefaultLocator(), extent).Finish();
    }
};
// The active render pass's prune collection (husk --pass), as for Storm.
class HdUE_RenderPassPruneSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return HdsiRenderPassPruneSceneIndex::New(input);
    }
};
// UsdGeomVisibilityAPI: UsdImaging publishes no purpose visibility and UE's USD importer ignores it (only
// ComputeVisibility, USDPrimConversion.cpp), so hdUE follows the spec (UsdGeomImageable::ComputeEffectiveVisibility):
// this adapter publishes the authored tokens, HdUEPurposeVisibility hides render/proxy/guide gprims they make
// invisible. ponytail: an edit on an ancestor does not dirty its descendants.
TF_DEFINE_PRIVATE_TOKENS(HdUEPurposeVisibilityTokens, ((container, "hdUE:purposeVisibility"))(render)(proxy)(guide));
class HdUE_VisibilityAPIAdapter final : public UsdImagingAPISchemaAdapter {
public:
    HdContainerDataSourceHandle GetImagingSubprimData(const UsdPrim& prim, const TfToken& subprim, const TfToken&,
                                                      const UsdImagingDataSourceStageGlobals&) override {
        if (!subprim.IsEmpty()) return nullptr;
        const UsdGeomVisibilityAPI api(prim);
        const auto token = [](const UsdAttribute& attribute) {
            TfToken value = UsdGeomTokens->inherited;
            attribute.Get(&value);
            return HdRetainedTypedSampledDataSource<TfToken>::New(value);
        };
        return HdRetainedContainerDataSource::New(
            HdUEPurposeVisibilityTokens->container,
            HdRetainedContainerDataSource::New(HdUEPurposeVisibilityTokens->render,
                                               token(api.GetRenderVisibilityAttr()), HdUEPurposeVisibilityTokens->proxy,
                                               token(api.GetProxyVisibilityAttr()), HdUEPurposeVisibilityTokens->guide,
                                               token(api.GetGuideVisibilityAttr())));
    }
    HdDataSourceLocatorSet InvalidateImagingSubprim(const UsdPrim&, const TfToken& subprim, const TfToken&,
                                                    const TfTokenVector& properties,
                                                    UsdImagingPropertyInvalidationType) override {
        for (const auto& property : properties)
            if (subprim.IsEmpty() && TfStringEndsWith(property.GetString(), "Visibility"))
                return HdDataSourceLocator(HdUEPurposeVisibilityTokens->container);
        return {};
    }
};
struct HdUEPurposeVisibility {
    static HdContainerDataSourceHandle edit(const HdSceneIndexBase& input, const SdfPath& path,
                                            const HdSceneIndexPrim& prim) {
        if (!HdPrimTypeIsGprim(prim.primType)) return nullptr;
        const auto purposeSource = HdPurposeSchema::GetFromParent(prim.dataSource).GetPurpose();
        const auto purpose = purposeSource ? purposeSource->GetTypedValue(0) : TfToken();
        if (purpose != HdUEPurposeVisibilityTokens->render && purpose != HdUEPurposeVisibilityTokens->proxy &&
            purpose != HdUEPurposeVisibilityTokens->guide)
            return nullptr;
        // The nearest authored (not inherited) value on the prim or an ancestor; guides fall back to invisible.
        bool visible = purpose != HdUEPurposeVisibilityTokens->guide;
        for (auto at = path; !at.IsEmpty() && !at.IsAbsoluteRootPath(); at = at.GetParentPath()) {
            const auto source = at == path ? prim.dataSource : input.GetPrim(at).dataSource;
            const auto value = HdTypedSampledDataSource<TfToken>::Cast(HdContainerDataSource::Get(
                source, HdDataSourceLocator(HdUEPurposeVisibilityTokens->container, purpose)));
            if (value && value->GetTypedValue(0) != UsdGeomTokens->inherited) {
                visible = value->GetTypedValue(0) == UsdGeomTokens->visible;
                break;
            }
        }
        if (visible) return nullptr;
        return HdContainerDataSourceEditor(prim.dataSource)
            .Set(
                HdVisibilitySchema::GetDefaultLocator(),
                HdVisibilitySchema::Builder().SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false)).Build())
            .Finish();
    }
};
class HdUE_PurposeVisibilitySceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return TfCreateRefPtr(new HdUE_EditingSceneIndex(input, HdUEPurposeVisibility::edit));
    }
};
// HermiteCurves: UsdImaging's (internal) adapter gives the scene index an untyped prim and UE's USD importer has no
// Hermite curves, so hdUE follows the spec with an adapter that overrides it: the gprim's own data, typed basisCurves
// and published as cubic Bezier curves (HdUEHermiteToBezier); vertex widths and displayColor become varying (one per
// point). ponytail: sampled at the stage time only (no deformation blur).
class HdUE_HermiteCurvesAdapter final : public UsdImagingHermiteCurvesAdapter {
public:
    TfToken GetImagingSubprimType(const UsdPrim&, const TfToken& subprim) override {
        return subprim.IsEmpty() ? HdPrimTypeTokens->basisCurves : TfToken();
    }
    HdContainerDataSourceHandle GetImagingSubprimData(const UsdPrim& prim, const TfToken& subprim,
                                                      const UsdImagingDataSourceStageGlobals& globals) override {
        const auto gprim = UsdImagingHermiteCurvesAdapter::GetImagingSubprimData(prim, subprim, globals);
        if (!subprim.IsEmpty()) return gprim;
        const auto curves = HermiteCurves(prim, globals);
        return curves ? HdOverlayContainerDataSource::New(curves, gprim) : gprim;
    }
    HdDataSourceLocatorSet InvalidateImagingSubprim(const UsdPrim& prim, const TfToken& subprim,
                                                    const TfTokenVector& properties,
                                                    UsdImagingPropertyInvalidationType type) override {
        auto locators = UsdImagingHermiteCurvesAdapter::InvalidateImagingSubprim(prim, subprim, properties, type);
        if (subprim.IsEmpty() && !properties.empty()) {
            locators.insert(HdBasisCurvesSchema::GetDefaultLocator());
            locators.insert(HdPrimvarsSchema::GetDefaultLocator());
        }
        return locators;
    }

private:
    static HdContainerDataSourceHandle HermiteCurves(const UsdPrim& prim,
                                                     const UsdImagingDataSourceStageGlobals& globals) {
        const UsdGeomHermiteCurves curves(prim);
        const auto time = globals.GetTime();
        VtVec3fArray points, tangents;
        VtIntArray counts, bezierCounts;
        curves.GetPointsAttr().Get(&points, time);
        curves.GetTangentsAttr().Get(&tangents, time);
        curves.GetCurveVertexCountsAttr().Get(&counts, time);
        const auto bezier = HdUEHermiteToBezier(points, tangents, counts, bezierCounts);
        if (bezier.empty()) return nullptr;
        const auto primvar = [](const VtValue& value, const TfToken& interpolation, const TfToken& role) {
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(HdRetainedSampledDataSource::New(value))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
                .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
                .Build();
        };
        // Per-point values sit on the segment ends: varying.
        const auto perPoint = [](const TfToken& interpolation) {
            return interpolation == UsdGeomTokens->vertex ? HdPrimvarSchemaTokens->varying : interpolation;
        };
        std::vector<TfToken> names{HdPrimvarsSchemaTokens->points};
        std::vector<HdDataSourceBaseHandle> values{
            primvar(VtValue(bezier), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point)};
        if (VtFloatArray widths; curves.GetWidthsAttr().Get(&widths, time)) {
            names.push_back(HdTokens->widths);
            values.push_back(primvar(VtValue(widths), perPoint(curves.GetWidthsInterpolation()), TfToken()));
        }
        if (const auto color = curves.GetDisplayColorPrimvar(); color && color.HasValue()) {
            VtVec3fArray colors;
            if (color.ComputeFlattened(&colors, time)) {
                names.push_back(HdTokens->displayColor);
                values.push_back(
                    primvar(VtValue(colors), perPoint(color.GetInterpolation()), HdPrimvarSchemaTokens->color));
            }
        }
        return HdRetainedContainerDataSource::New(
            HdBasisCurvesSchema::GetSchemaToken(),
            HdBasisCurvesSchema::Builder()
                .SetTopology(HdBasisCurvesTopologySchema::Builder()
                                 .SetCurveVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(bezierCounts))
                                 .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->bezier))
                                 .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->cubic))
                                 .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->nonperiodic))
                                 .Build())
                .Build(),
            HdPrimvarsSchema::GetSchemaToken(),
            HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()));
    }
};
// The active render pass's renderVisibility collection (husk --pass): geometry outside it is invisible. Storm's own
// (HdSt_RenderPassVisibilitySceneIndexPlugin) is registered for GL only and hdsi has none; UE's USD importer has no
// render passes. ponytail: re-reads the pass when scene globals or the pass prim change, then dirties every prim's
// visibility.
class HdUE_RenderPassVisibilitySceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    explicit HdUE_RenderPassVisibilitySceneIndex(const HdSceneIndexBaseRefPtr& input)
        : HdSingleInputFilteringSceneIndexBase(input) {
        update();
    }
    HdSceneIndexPrim GetPrim(const SdfPath& path) const override {
        auto prim = _GetInputSceneIndex()->GetPrim(path);
        std::lock_guard lock(mutex_);
        if (prim.dataSource && eval_ && HdPrimTypeIsGprim(prim.primType) && !eval_->Match(path))
            prim.dataSource = HdContainerDataSourceEditor(prim.dataSource)
                                  .Set(HdVisibilitySchema::GetDefaultLocator(),
                                       HdVisibilitySchema::Builder()
                                           .SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false))
                                           .Build())
                                  .Finish();
        return prim;
    }
    SdfPathVector GetChildPrimPaths(const SdfPath& path) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    void _PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) override {
        const bool changed =
            std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return affects(e.primPath); });
        _SendPrimsAdded(entries);
        if (changed && update()) dirtyAll();
    }
    void _PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) override {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) override {
        const bool changed =
            std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return affects(e.primPath); });
        _SendPrimsDirtied(entries);
        if (changed && update()) dirtyAll();
    }

private:
    bool affects(const SdfPath& path) const {
        std::lock_guard lock(mutex_);
        return path.IsAbsoluteRootPath() || path == pass_;
    }
    // Reads the active pass's collection; whether the membership changed.
    bool update() {
        const auto input = _GetInputSceneIndex();
        const auto active = HdSceneGlobalsSchema::GetFromSceneIndex(input).GetActiveRenderPassPrim();
        const auto pass = active ? active->GetTypedValue(0) : SdfPath();
        SdfPathExpression expression;
        std::optional<HdCollectionExpressionEvaluator> eval;
        if (!pass.IsEmpty()) {
            auto collections = HdCollectionsSchema::GetFromParent(input->GetPrim(pass).dataSource);
            HdsiUtilsCompileCollection(collections, TfToken("renderVisibility"), input, &expression, &eval);
        }
        std::lock_guard lock(mutex_);
        const bool changed = pass != pass_ || expression != expression_;
        pass_ = pass;
        expression_ = expression;
        eval_ = std::move(eval);
        return changed;
    }
    void dirtyAll() {
        HdSceneIndexObserver::DirtiedPrimEntries dirty;
        for (const auto& path : HdSceneIndexPrimView(_GetInputSceneIndex()))
            dirty.emplace_back(path, HdVisibilitySchema::GetDefaultLocator());
        _SendPrimsDirtied(dirty);
    }
    mutable std::mutex mutex_;
    SdfPath pass_;
    SdfPathExpression expression_;
    std::optional<HdCollectionExpressionEvaluator> eval_;
};
class HdUE_RenderPassVisibilitySceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return TfCreateRefPtr(new HdUE_RenderPassVisibilitySceneIndex(input));
    }
};
// Says once what the stage authors that hdUE (like UE's USD importer) does not render, where Hydra would otherwise drop
// it silently; prims pass through unchanged. HDUE_DUMP_PRIMS=1 prints every added prim (debugging).
class HdUE_UnsupportedSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    explicit HdUE_UnsupportedSceneIndex(const HdSceneIndexBaseRefPtr& input)
        : HdSingleInputFilteringSceneIndexBase(input) {}
    HdSceneIndexPrim GetPrim(const SdfPath& path) const override { return _GetInputSceneIndex()->GetPrim(path); }
    SdfPathVector GetChildPrimPaths(const SdfPath& path) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    void _PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) override {
        static const bool dump = TfGetenvBool("HDUE_DUMP_PRIMS", false);
        for (const auto& entry : entries) {
            const auto prim = _GetInputSceneIndex()->GetPrim(entry.primPath);
            if (!prim.dataSource) continue;
            if (dump) {
                std::cerr << entry.primPath << " (" << entry.primType << ")" << std::endl;
                HdDebugPrintDataSource(std::cerr, prim.dataSource, 1);
            }
            if (const auto schemas = UsdImagingUsdPrimInfoSchema::GetFromParent(prim.dataSource).GetApiSchemas())
                for (const auto& schema : schemas->GetTypedValue(0)) {
                    if (schema == "VolumeLightAPI")
                        warn(schema, "%s is not supported: %s renders without emitting light", schema.GetText(),
                             entry.primPath.GetText());
                }
        }
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) override {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) override {
        _SendPrimsDirtied(entries);
    }

private:
    template <class... A>
    void warn(const TfToken& key, const char* format, A... args) {
        static std::mutex mutex;
        static std::set<TfToken> warned;
        std::lock_guard lock(mutex);
        if (warned.insert(key).second) TF_WARN("hdUE: %s", TfStringPrintf(format, args...).c_str());
    }
};
class HdUE_UnsupportedSceneIndexPlugin final : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                             const HdContainerDataSourceHandle&) override {
        return TfCreateRefPtr(new HdUE_UnsupportedSceneIndex(input));
    }
};
TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<HdUE_UnsupportedSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_DrawModeSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_PurposeVisibilitySceneIndexPlugin>();
    TfType::Define<HdUE_HermiteCurvesAdapter, TfType::Bases<UsdImagingHermiteCurvesAdapter>>()
        .SetFactory<UsdImagingPrimAdapterFactory<HdUE_HermiteCurvesAdapter>>();
    TfType::Define<HdUE_VisibilityAPIAdapter, TfType::Bases<UsdImagingAPISchemaAdapter>>()
        .SetFactory<UsdImagingAPISchemaAdapterFactory<HdUE_VisibilityAPIAdapter>>();
    HdSceneIndexPluginRegistry::Define<HdUE_RenderPassVisibilitySceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_ConversionSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_RenderPassPruneSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_ImplicitSurfaceSceneIndexPlugin>();
    HdSceneIndexPluginRegistry::Define<HdUE_LightLinkingSceneIndexPlugin>();
}
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    // The renderer display name from plugInfo.json.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_ImplicitSurfaceSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_LightLinkingSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_ConversionSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_RenderPassPruneSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_DrawModeSceneIndexPlugin"), nullptr, 1,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_UnsupportedSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_PurposeVisibilitySceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Unreal Engine (hdUE)", TfToken("HdUE_RenderPassVisibilitySceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}
PXR_NAMESPACE_CLOSE_SCOPE
