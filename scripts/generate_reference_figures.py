"""Write the render settings reference's figure specs (docs/figures/ref-*.json) from one table: for each setting, the
scene that shows it and the values to compare. Each spec names its setting ("settings"), and
generate_render_settings.py puts the figure under that setting in docs/reference/. Hand-written figures of the guide
pages name their settings the same way.

    uv run python scripts/generate_reference_figures.py
    hython scripts/render_docs_images.py --only <figures>          # then render them

Values are USD values as authored in the root layer; a caption marked * is UE's default.
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "docs" / "figures"
SETTINGS = "/Render/Settings"
# From out/docs-images/<figure>/ (where the root layers are written) back to the repository.
DIRT = "@../../../samples/docs/textures/lens_dirt.png@"

# Scenes: (layer under samples/, base overrides).
LOOK = ("docs/lookdev.usda", {})
LIGHTS = ("docs/lights.usda", {})
# The light study with its key 3 stops up and the exposure 2.5 down: the chrome ball's highlight is bright enough to
# bloom, flare and light the lens dirt, the rest of the frame as before.
HOT = (
    "docs/lights.usda",
    {
        "/scene/key": ["float inputs:exposure = 7"],
        SETTINGS: ["float unreal:postProcess:AutoExposureBias = -2.5"],
    },
)
FOG = ("docs/fog.usda", {})
BISTRO = (
    "docs/bistro.usda",
    {SETTINGS: ['token unreal:renderMode = "realtime"']},
)
NO_LUMEN_GI = (
    "docs/bistro.usda",
    {
        SETTINGS: [
            'token unreal:renderMode = "realtime"',
            'token unreal:postProcess:DynamicGlobalIlluminationMethod = "EDynamicGlobalIlluminationMethod::None"',
        ]
    },
)
CORNELL_PT = (
    "cornell/cornell.usda",
    {SETTINGS: ['token unreal:renderMode = "pathtraced"']},
)
SUN = {
    "/scene/sky": ["(active = false)"],
    "/scene/key": ["(active = false)"],
    "/scene/sun": [
        "(active = true)",
        "float3 xformOp:rotateXYZ = (-15, 170, 0)",
    ],
    "/scene/camera": ["float3 xformOp:rotateXYZ = (0, 0, 0)"],
    SETTINGS: ["bool unreal:skyAtmosphere = 1"],
}
SKY = ("docs/lights.usda", SUN)
CLOUDS = (
    "docs/lights.usda",
    {**SUN, SETTINGS: SUN[SETTINGS] + ["bool unreal:volumetricCloud = 1"]},
)
SUN_SHADOW = (
    "docs/lights.usda",
    {"/scene/key": ["(active = false)"], "/scene/sun": ["(active = true)"]},
)
ROW_DOF = (
    "docs/row.usda",
    {
        "/scene/camera": [
            "float focalLength = 0.5",
            "float horizontalAperture = 0.36",
            "float verticalAperture = 0.2025",
            "float fStop = 1.4",
            "float focusDistance = 2.4",
        ]
    },
)
UNLIT = (
    "docs/lights.usda",
    {"/scene/sky": ["(active = false)"], "/scene/key": ["(active = false)"]},
)
LOCAL_FOG = (
    "docs/lights.usda",
    {
        "/scene/localFog": ["(active = true)"],
        # UE draws no local fog volume within 10 m of the camera by default; the camera is 4 m away.
        SETTINGS: [
            "float unreal:cvar:r:LocalFogVolume:GlobalStartDistance = 0"
        ],
    },
)


def with_base(scene, *lines, prim=SETTINGS):
    layer, base = scene
    merged = {k: list(v) for k, v in base.items()}
    merged.setdefault(prim, []).extend(lines)
    return layer, merged


BLOOM_FFT = with_base(HOT, 'token unreal:postProcess:BloomMethod = "BM_FFT"')
DIRTY = with_base(
    HOT,
    f"asset unreal:postProcess:BloomDirtMask = {DIRT}",
    "float unreal:postProcess:BloomIntensity = 2",
)
FLARES = with_base(HOT, "float unreal:postProcess:LensFlareIntensity = 4")
FRINGE = with_base(LIGHTS, "float unreal:postProcess:SceneFringeIntensity = 5")
GRAIN = with_base(LOOK, "float unreal:postProcess:FilmGrainIntensity = 1")
HISTOGRAM = with_base(
    BISTRO, 'token unreal:postProcess:AutoExposureMethod = "AEM_Histogram"'
)
# The shader balls are lit for EV100 0, and ISO 100, 1/60 s, f/4 is EV100 9.9: +9 stops of compensation bring them back,
# so the figure shows what the aperture changes.
PHYSICAL = with_base(
    LOOK,
    "bool unreal:postProcess:AutoExposureApplyPhysicalCameraExposure = 1",
    "float unreal:postProcess:AutoExposureBias = 9",
)
CLOUD_SHADOWS = with_base(
    CLOUDS, "bool unreal:light:bCastCloudShadows = 1", prim="/scene/sun"
)


def grade(attribute, values):
    return [
        (c, f"float4 unreal:postProcess:{attribute} = ({v})") for c, v in values
    ]


# (figure, scene, setting attribute, [(caption, line)], prim the lines go on).
TABLE = []


def add(name, scene, attribute, values, prim=SETTINGS):
    TABLE.append((name, scene, attribute, values, prim))


def pp(name, scene, attribute, kind, values, prim=SETTINGS):
    """A post-process (or other unreal:) setting: values as (caption, USD value)."""
    add(
        name,
        scene,
        attribute,
        [(c, f"{kind} {attribute} = {v}") for c, v in values],
        prim,
    )


P = "unreal:postProcess:"
# Colour grading, on the shader balls.
pp(
    "ref-white-tint",
    LOOK,
    P + "WhiteTint",
    "float",
    [("-0.5", "-0.5"), ("0*", "0"), ("0.5", "0.5")],
)
add(
    "ref-temperature-type",
    with_base(LOOK, f"float {P}WhiteTemp = 4500"),
    P + "TemperatureType",
    [
        (
            "White Balance*, 4500 K",
            f'token {P}TemperatureType = "TEMP_WhiteBalance"',
        ),
        (
            "Color Temperature, 4500 K",
            f'token {P}TemperatureType = "TEMP_ColorTemperature"',
        ),
    ],
)
# Shadows: the shader balls have too few dark tones for the shadow range to show; ShadowsMax shows it instead.
for region in ("", "Midtones", "Highlights"):
    key = f"-{region.lower()}" if region else ""
    if region:
        add(
            f"ref-saturation{key}",
            LOOK,
            P + "ColorSaturation" + region,
            grade(
                "ColorSaturation" + region,
                [("1, 1, 1, 1*", "1, 1, 1, 1"), ("1, 1, 1, 0", "1, 1, 1, 0")],
            ),
        )
    add(
        f"ref-contrast{key}",
        LOOK,
        P + "ColorContrast" + region,
        grade(
            "ColorContrast" + region,
            [
                ("1, 1, 1, 0.7", "1, 1, 1, 0.7"),
                ("1, 1, 1, 1*", "1, 1, 1, 1"),
                ("1, 1, 1, 1.4", "1, 1, 1, 1.4"),
            ],
        ),
    )
    add(
        f"ref-gamma{key}",
        LOOK,
        P + "ColorGamma" + region,
        grade(
            "ColorGamma" + region,
            [
                ("1, 1, 1, 0.7", "1, 1, 1, 0.7"),
                ("1, 1, 1, 1*", "1, 1, 1, 1"),
                ("1, 1, 1, 1.4", "1, 1, 1, 1.4"),
            ],
        ),
    )
    add(
        f"ref-gain{key}",
        LOOK,
        P + "ColorGain" + region,
        grade(
            "ColorGain" + region,
            [
                ("1, 1, 1, 1*", "1, 1, 1, 1"),
                ("1.2, 1, 0.8, 1", "1.2, 1, 0.8, 1"),
                ("0.8, 0.9, 1.3, 1", "0.8, 0.9, 1.3, 1"),
            ],
        ),
    )
    add(
        f"ref-offset{key}",
        LOOK,
        P + "ColorOffset" + region,
        grade(
            "ColorOffset" + region,
            [
                ("0, 0, 0, 0*", "0, 0, 0, 0"),
                ("0, 0, 0, 0.02", "0, 0, 0, 0.02"),
                ("0, 0, 0.02, 0", "0, 0, 0.02, 0"),
            ],
        ),
    )
pp(
    "ref-shadows-max",
    with_base(LOOK, f"float4 {P}ColorSaturationShadows = (1, 1, 1, 0)"),
    P + "ColorCorrectionShadowsMax",
    "float",
    [("0.09*", "0.09"), ("0.3", "0.3")],
)
pp(
    "ref-highlights-min",
    with_base(LOOK, f"float4 {P}ColorSaturationHighlights = (1, 1, 1, 0)"),
    P + "ColorCorrectionHighlightsMin",
    "float",
    [("0.5*", "0.5"), ("0.2", "0.2")],
)
pp(
    "ref-tone-curve-amount",
    LOOK,
    P + "ToneCurveAmount",
    "float",
    [("0", "0"), ("0.5", "0.5"), ("1*", "1")],
)
pp(
    "ref-scene-color-tint",
    LOOK,
    P + "SceneColorTint",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.85, 0.7", "(1, 0.85, 0.7)")],
)
pp(
    "ref-film-slope",
    LOOK,
    P + "FilmSlope",
    "float",
    [("0.6", "0.6"), ("0.88*", "0.88"), ("1", "1")],
)
pp(
    "ref-film-toe",
    LOOK,
    P + "FilmToe",
    "float",
    [("0.3", "0.3"), ("0.55*", "0.55"), ("0.8", "0.8")],
)
pp(
    "ref-film-shoulder",
    LOOK,
    P + "FilmShoulder",
    "float",
    [("0", "0"), ("0.26*", "0.26"), ("0.6", "0.6")],
)
pp(
    "ref-film-black-clip",
    LOOK,
    P + "FilmBlackClip",
    "float",
    [("0*", "0"), ("0.02", "0.02")],
)
pp(
    "ref-film-white-clip",
    LOOK,
    P + "FilmWhiteClip",
    "float",
    [("0", "0"), ("0.04*", "0.04"), ("0.2", "0.2")],
)
for region in ("Shadows", "Midtones", "Highlights"):
    pp(
        f"ref-film-grain-{region.lower()}",
        GRAIN,
        f"{P}FilmGrainIntensity{region}",
        "float",
        [("1*", "1"), ("0", "0")],
    )
pp(
    "ref-film-grain-scale",
    GRAIN,
    P + "FilmGrainScale",
    "float2",
    [("1, 1*", "(1, 1)"), ("3, 3", "(3, 3)")],
)

# Lens, on the light study: the chrome ball's highlight is the bright source.
pp(
    "ref-bloom-method",
    HOT,
    P + "BloomMethod",
    "token",
    [("Standard*", '"BM_SOG"'), ("Convolution", '"BM_FFT"')],
)
pp(
    "ref-bloom-size-scale",
    HOT,
    P + "BloomSizeScale",
    "float",
    [("1", "1"), ("4*", "4"), ("16", "16")],
)
pp(
    "ref-bloom-convolution-intensity",
    BLOOM_FFT,
    P + "BloomConvolutionIntensity",
    "float",
    [("0.5", "0.5"), ("1*", "1"), ("2", "2")],
)
pp(
    "ref-bloom-convolution-size",
    BLOOM_FFT,
    P + "BloomConvolutionSize",
    "float",
    [("0.5", "0.5"), ("1*", "1")],
)
add(
    "ref-dirt-mask-texture",
    with_base(
        HOT,
        "float unreal:postProcess:BloomIntensity = 2",
        f"float {P}BloomDirtMaskIntensity = 20",
    ),
    P + "BloomDirtMask",
    [
        ("(empty)*", f"asset {P}BloomDirtMask = @@"),
        ("lens_dirt.png", f"asset {P}BloomDirtMask = {DIRT}"),
    ],
)
pp(
    "ref-dirt-mask-intensity",
    DIRTY,
    P + "BloomDirtMaskIntensity",
    "float",
    [("0*", "0"), ("10", "10"), ("40", "40")],
)
pp(
    "ref-dirt-mask-tint",
    with_base(DIRTY, f"float {P}BloomDirtMaskIntensity = 20"),
    P + "BloomDirtMaskTint",
    "color3f",
    [("0.5, 0.5, 0.5*", "(0.5, 0.5, 0.5)"), ("1, 0.6, 0.3", "(1, 0.6, 0.3)")],
)
pp(
    "ref-chromatic-aberration-offset",
    FRINGE,
    P + "ChromaticAberrationStartOffset",
    "float",
    [("0*", "0"), ("0.5", "0.5")],
)
pp(
    "ref-lens-flare-tint",
    FLARES,
    P + "LensFlareTint",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.5, 0.2", "(1, 0.5, 0.2)")],
)
pp(
    "ref-lens-flare-bokeh-size",
    FLARES,
    P + "LensFlareBokehSize",
    "float",
    [("1", "1"), ("3*", "3"), ("8", "8")],
)
pp(
    "ref-sharpen",
    LOOK,
    P + "Sharpen",
    "float",
    [("0*", "0"), ("2", "2"), ("6", "6")],
)
pp(
    "ref-min-brightness",
    HISTOGRAM,
    P + "AutoExposureMinBrightness",
    "float",
    [("0.03*", "0.03"), ("1", "1")],
)
pp(
    "ref-max-brightness",
    HISTOGRAM,
    P + "AutoExposureMaxBrightness",
    "float",
    [("0.1", "0.1"), ("8*", "8")],
)
pp(
    "ref-low-percent",
    HISTOGRAM,
    P + "AutoExposureLowPercent",
    "float",
    [("10*", "10"), ("70", "70")],
)
pp(
    "ref-high-percent",
    HISTOGRAM,
    P + "AutoExposureHighPercent",
    "float",
    [("50", "50"), ("90*", "90")],
)
pp(
    "ref-camera-aperture",
    PHYSICAL,
    P + "DepthOfFieldFstop",
    "float",
    [("2", "2"), ("4*", "4"), ("8", "8")],
)
pp(
    "ref-local-exposure-method",
    BISTRO,
    P + "LocalExposureMethod",
    "token",
    [
        ("Bilateral*", '"ELocalExposureMethod::Bilateral"'),
        ("Fusion", '"ELocalExposureMethod::Fusion"'),
    ],
)
pp(
    "ref-local-exposure-highlights",
    BISTRO,
    P + "LocalExposureHighlightContrastScale",
    "float",
    [("1*", "1"), ("0.5", "0.5")],
)
pp(
    "ref-local-exposure-shadows",
    BISTRO,
    P + "LocalExposureShadowContrastScale",
    "float",
    [("1*", "1"), ("0.5", "0.5")],
)
pp(
    "ref-local-exposure-detail",
    BISTRO,
    P + "LocalExposureDetailStrength",
    "float",
    [("1*", "1"), ("2", "2")],
)
pp(
    "ref-dof-enabled",
    ROW_DOF,
    P + "DepthOfFieldEnabled",
    "bool",
    [("On*", "1"), ("Off", "0")],
)
pp(
    "ref-dof-squeeze",
    ROW_DOF,
    P + "DepthOfFieldSqueezeFactor",
    "float",
    [("1*", "1"), ("2", "2")],
)
pp(
    "ref-dof-petzval",
    ROW_DOF,
    P + "DepthOfFieldPetzvalBokeh",
    "float",
    [("0*", "0"), ("2", "2")],
)

# Lighting: the Bistro interior in Realtime.
pp(
    "ref-lumen-enabled",
    BISTRO,
    "unreal:lumenEnabled",
    "bool",
    [("On*", "1"), ("Off", "0")],
)
pp(
    "ref-indirect-lighting-color",
    BISTRO,
    P + "IndirectLightingColor",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.7, 0.5", "(1, 0.7, 0.5)")],
)
pp(
    "ref-indirect-lighting-intensity",
    BISTRO,
    P + "IndirectLightingIntensity",
    "float",
    [("0", "0"), ("1*", "1"), ("2", "2")],
)
pp(
    "ref-lumen-scene-detail",
    BISTRO,
    P + "LumenSceneDetail",
    "float",
    [("0.25", "0.25"), ("1*", "1"), ("4", "4")],
)
pp(
    "ref-lumen-screen-traces",
    BISTRO,
    P + "LumenFinalGatherScreenTraces",
    "bool",
    [("On*", "1"), ("Off", "0")],
)
pp(
    "ref-lumen-max-trace-distance",
    BISTRO,
    P + "LumenMaxTraceDistance",
    "float",
    [("200", "200"), ("20000*", "20000")],
)
pp(
    "ref-lumen-diffuse-color-boost",
    BISTRO,
    P + "LumenDiffuseColorBoost",
    "float",
    [("1*", "1"), ("2", "2")],
)
pp(
    "ref-lumen-ao-intensity",
    BISTRO,
    P + "LumenAmbientOcclusionIntensity",
    "float",
    [("0", "0"), ("1*", "1")],
)
pp(
    "ref-lumen-skylight-leaking",
    BISTRO,
    P + "LumenSkylightLeaking",
    "float",
    [("0*", "0"), ("0.3", "0.3")],
)
pp(
    "ref-lumen-hardware-ray-tracing",
    BISTRO,
    "unreal:cvar:r:Lumen:HardwareRayTracing",
    "bool",
    [("On*", "1"), ("Off (software)", "0")],
)
pp(
    "ref-reflection-method",
    LOOK,
    P + "ReflectionMethod",
    "token",
    [
        ("Lumen*", '"EReflectionMethod::Lumen"'),
        ("Screen Space", '"EReflectionMethod::ScreenSpace"'),
        ("None", '"EReflectionMethod::None"'),
    ],
)
pp(
    "ref-lumen-reflection-screen-traces",
    LOOK,
    P + "LumenReflectionsScreenTraces",
    "bool",
    [("On*", "1"), ("Off", "0")],
)
pp(
    "ref-megalights",
    BISTRO,
    P + "bMegaLights",
    "bool",
    [("On*", "1"), ("Off", "0")],
)
pp(
    "ref-ssao-intensity",
    NO_LUMEN_GI,
    P + "AmbientOcclusionIntensity",
    "float",
    [("0", "0"), ("0.5*", "0.5"), ("1", "1")],
)
pp(
    "ref-ssao-radius",
    with_base(NO_LUMEN_GI, f"float {P}AmbientOcclusionIntensity = 1"),
    P + "AmbientOcclusionRadius",
    "float",
    [("50", "50"), ("200*", "200"), ("600", "600")],
)
pp(
    "ref-rtao",
    NO_LUMEN_GI,
    P + "RayTracingAO",
    "bool",
    [("Off*", "0"), ("On", "1")],
)
pp(
    "ref-virtual-shadow-maps",
    SUN_SHADOW,
    "unreal:cvar:r:Shadow:Virtual:Enable",
    "bool",
    [("On*", "1"), ("Off (shadow maps)", "0")],
)
pp(
    "ref-ray-traced-shadows",
    SUN_SHADOW,
    "unreal:cvar:r:RayTracing:Shadows",
    "bool",
    [("Off*", "0"), ("On", "1")],
)
pp(
    "ref-vsm-directional-bias",
    SUN_SHADOW,
    "unreal:cvar:r:Shadow:Virtual:ResolutionLodBiasDirectional",
    "float",
    [("-1.5*", "-1.5"), ("2", "2")],
)

# Rendering.
pp(
    "ref-anti-aliasing",
    LIGHTS,
    "unreal:cvar:r:AntiAliasingMethod",
    "token",
    [("None", '"0"'), ("FXAA", '"1"'), ("TSR*", '"4"')],
)
pp(
    "ref-screen-percentage",
    LIGHTS,
    "unreal:cvar:r:ScreenPercentage",
    "float",
    [("25", "25"), ("50", "50"), ("100*", "100")],
)
pp(
    "ref-shadow-quality",
    SUN_SHADOW,
    "unreal:cvar:sg:ShadowQuality",
    "token",
    [("Low", '"0"'), ("High", '"2"'), ("Epic*", '"3"')],
)
pp(
    "ref-pt-max-bounces",
    CORNELL_PT,
    P + "PathTracingMaxBounces",
    "int",
    [("1", "1"), ("2", "2"), ("32*", "32")],
)
for part in ("Diffuse", "IndirectDiffuse", "Specular", "IndirectSpecular"):
    name = "ref-pt-include-" + part.lower().replace("indirect", "indirect-")
    pp(
        name,
        CORNELL_PT,
        f"{P}PathTracingInclude{part}",
        "bool",
        [("On*", "1"), ("Off", "0")],
    )
pp(
    "ref-motion-blur-amount",
    (
        "cornell/variants/motion_blur.usda",
        {SETTINGS: ['token unreal:renderMode = "realtime"']},
    ),
    P + "MotionBlurAmount",
    "float",
    [("0.25", "0.25"), ("0.5*", "0.5"), ("1", "1")],
)

# Fog: the fog room.
pp(
    "ref-height-fog",
    FOG,
    "unreal:heightFog",
    "bool",
    [("Off*", "0"), ("On", "1")],
)
H = "unreal:heightFog:"
pp(
    "ref-second-fog-density",
    with_base(FOG, f"float {H}SecondFogData:FogHeightFalloff = 2"),
    H + "SecondFogData:FogDensity",
    "float",
    [("0*", "0"), ("0.5", "0.5")],
)
pp(
    "ref-volumetric-fog-albedo",
    FOG,
    H + "VolumetricFogAlbedo",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.6, 0.3", "(1, 0.6, 0.3)")],
)
pp(
    "ref-volumetric-fog-emissive",
    FOG,
    H + "VolumetricFogEmissive",
    "color3f",
    [("0, 0, 0*", "(0, 0, 0)"), ("0.02, 0.01, 0", "(0.02, 0.01, 0)")],
)
pp(
    "ref-volumetric-fog-start-distance",
    FOG,
    H + "VolumetricFogStartDistance",
    "float",
    [("0*", "0"), ("300", "300")],
)

# Sky and clouds: the light study under UE's sky, the sun low.
S = "unreal:skyAtmosphere:"
pp(
    "ref-sky-atmosphere",
    with_base(SKY, "bool unreal:skyAtmosphere = 0"),
    "unreal:skyAtmosphere",
    "bool",
    [("Off*", "0"), ("On", "1")],
)
pp(
    "ref-rayleigh-scale",
    SKY,
    S + "RayleighScatteringScale",
    "float",
    [("0.01", "0.01"), ("0.0331*", "0.0331"), ("0.1", "0.1")],
)
pp(
    "ref-mie-anisotropy",
    SKY,
    S + "MieAnisotropy",
    "float",
    [("0.4", "0.4"), ("0.8*", "0.8"), ("0.95", "0.95")],
)
pp(
    "ref-mie-absorption-scale",
    SKY,
    S + "MieAbsorptionScale",
    "float",
    [("0.000444*", "0.000444"), ("0.01", "0.01")],
)
pp(
    "ref-ozone-absorption-scale",
    SKY,
    S + "OtherAbsorptionScale",
    "float",
    [("0", "0"), ("0.001881*", "0.001881"), ("0.01", "0.01")],
)
pp(
    "ref-atmosphere-height",
    SKY,
    S + "AtmosphereHeight",
    "float",
    [("20", "20"), ("60*", "60")],
)
pp(
    "ref-multiscattering",
    SKY,
    S + "MultiScatteringFactor",
    "float",
    [("0", "0"), ("1*", "1")],
)
pp(
    "ref-ground-albedo",
    SKY,
    S + "GroundAlbedo",
    "color3f",
    [
        ("0.4, 0.4, 0.4*", "(0.401978, 0.401978, 0.401978)"),
        ("0.8, 0.4, 0.2", "(0.8, 0.4, 0.2)"),
    ],
)
pp(
    "ref-sky-luminance-factor",
    SKY,
    S + "SkyLuminanceFactor",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.8, 0.6", "(1, 0.8, 0.6)")],
)
C = "unreal:volumetricCloud:"
pp(
    "ref-cloud-bottom-altitude",
    CLOUDS,
    C + "LayerBottomAltitude",
    "float",
    [("2", "2"), ("5*", "5")],
)
pp(
    "ref-cloud-layer-height",
    CLOUDS,
    C + "LayerHeight",
    "float",
    [("3", "3"), ("10*", "10")],
)
pp(
    "ref-cloud-view-samples",
    CLOUDS,
    C + "ViewSampleCountScale",
    "float",
    [("0.25", "0.25"), ("1*", "1"), ("4", "4")],
)

# Light properties.
L = "unreal:light:"
pp(
    "ref-light-cast-cloud-shadows",
    CLOUDS,
    L + "bCastCloudShadows",
    "bool",
    [("Off*", "0"), ("On", "1")],
    prim="/scene/sun",
)
pp(
    "ref-light-cloud-shadow-strength",
    CLOUD_SHADOWS,
    L + "CloudShadowStrength",
    "float",
    [("0.25", "0.25"), ("1*", "1")],
    prim="/scene/sun",
)

# Local Fog Volume.
F = "unreal:localFogVolume:"
LF = "/scene/localFog"
pp(
    "ref-lfv-radial-density",
    LOCAL_FOG,
    F + "RadialFogExtinction",
    "float",
    [("0.25", "0.25"), ("1*", "1"), ("2", "2")],
    prim=LF,
)
pp(
    "ref-lfv-height-density",
    LOCAL_FOG,
    F + "HeightFogExtinction",
    "float",
    [("0.01", "0.01"), ("1*", "1")],
    prim=LF,
)
pp(
    "ref-lfv-height-falloff",
    LOCAL_FOG,
    F + "HeightFogFalloff",
    "float",
    [("100", "100"), ("1000*", "1000")],
    prim=LF,
)
pp(
    "ref-lfv-phase",
    LOCAL_FOG,
    F + "FogPhaseG",
    "float",
    [("0", "0"), ("0.2*", "0.2"), ("0.8", "0.8")],
    prim=LF,
)
pp(
    "ref-lfv-albedo",
    LOCAL_FOG,
    F + "FogAlbedo",
    "color3f",
    [("1, 1, 1*", "(1, 1, 1)"), ("1, 0.5, 0.2", "(1, 0.5, 0.2)")],
    prim=LF,
)
pp(
    "ref-lfv-emissive",
    LOCAL_FOG,
    F + "FogEmissive",
    "color3f",
    [("0, 0, 0*", "(0, 0, 0)"), ("0.2, 0.05, 0", "(0.2, 0.05, 0)")],
    prim=LF,
)


def main():
    for old in SPECS.glob("ref-*.json"):
        old.unlink()
    for name, (layer, base), attribute, values, prim in TABLE:
        spec = {
            "scene": layer,
            "display": "tonemapped",
            "settings": [attribute],
            "base": base,
            "values": [
                {"caption": c.replace("*", " (default)"), "set": {prim: [line]}}
                for c, line in values
            ],
        }
        (SPECS / f"{name}.json").write_text(
            json.dumps(spec, indent=1) + "\n", encoding="utf-8", newline="\n"
        )
    print(f"{len(TABLE)} reference figures")


if __name__ == "__main__":
    main()
