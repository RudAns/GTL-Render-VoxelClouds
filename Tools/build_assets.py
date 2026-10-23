"""Build the assets the demo needs, headless:

  UnrealEditor-Cmd.exe VoxelClouds.uproject -run=pythonscript -script="<abs path>/Tools/build_assets.py"

Creates under /Game/CloudLab: the materials (terrain, sea, paint, canopy, wingtip trail, checkpoint ring, and the
Volume-domain material used when UE's own Volumetric Cloud component draws our voxel data), imports the
textures and sounds from SourceAssets, and saves the empty map /Game/Maps/Skies (the game mode builds the
world at runtime). Existing assets are kept; delete one to rebuild it.
"""
import os
import struct
import sys
import zlib
import unreal

ROOT = "/Game/CloudLab"
MAT = ROOT + "/Materials"
TEX = ROOT + "/Textures"
AUD = ROOT + "/Audio"
AT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(PROJ, "SourceAssets")
F1 = unreal.CustomMaterialOutputType.CMOT_FLOAT1
F2 = unreal.CustomMaterialOutputType.CMOT_FLOAT2
F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT3
F4 = unreal.CustomMaterialOutputType.CMOT_FLOAT4


def log(*a):
    unreal.log("[build_assets] " + " ".join(str(x) for x in a))


class Exists(Exception):
    pass


def fresh(name, cls, factory, path=MAT):
    full = f"{path}/{name}"
    if EAL.does_asset_exist(full):
        raise Exists(full)
    return AT.create_asset(name, path, cls, factory)


def build(fn, *args, **kw):
    try:
        fn(*args, **kw)
    except Exists as e:
        log("kept", e)


def set_flags(mat, **flags):
    for k, v in flags.items():
        try:
            mat.set_editor_property(k, v)
        except Exception as e:  # noqa
            log("flag", k, e)


class Graph:
    def __init__(self, mat):
        self.mat = mat
        self.y = 0

    def node(self, cls, x=-1200, **props):
        e = MEL.create_material_expression(self.mat, cls, x, self.y)
        self.y += 120
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def custom(self, code, inputs, out_type, desc, x=-400):
        c = self.node(unreal.MaterialExpressionCustom, x)
        c.set_editor_property("code", code)
        c.set_editor_property("description", desc)
        c.set_editor_property("output_type", out_type)
        ins = []
        for name, _ in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", name)
            ins.append(ci)
        c.set_editor_property("inputs", ins)
        for name, (src, out) in inputs:
            MEL.connect_material_expressions(src, out, c, name)
        return c


def import_texture(path, kind):
    name = os.path.splitext(os.path.basename(path))[0]
    full = f"{TEX}/{name}"
    if not EAL.does_asset_exist(full):
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", path)
        t.set_editor_property("destination_path", TEX)
        t.set_editor_property("automated", True)
        t.set_editor_property("save", False)
        t.set_editor_property("factory", unreal.TextureFactory())   # legacy factory: no absolute source path in the asset
        AT.import_asset_tasks([t])
    tex = unreal.load_asset(full)
    if tex is None:
        log("texture import failed", path)
        return None
    if kind != "diff":
        tex.set_editor_property("srgb", False)
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS if kind == "arm" else unreal.TextureCompressionSettings.TC_NORMALMAP)
    tex.set_editor_property("max_texture_size", 2048)
    EAL.save_loaded_asset(tex)
    return tex


def tex_set(name):
    d = os.path.join(SRC, "textures", name)
    files = os.listdir(d)
    pick = lambda k: os.path.join(d, next(f for f in files if f"_{k}_" in f))
    return import_texture(pick("diff"), "diff"), import_texture(pick("nor_gl"), "nor"), import_texture(pick("arm"), "arm")


# ---------------------------------------------------------------- terrain
TERRAIN = r"""
// slope and height decide sand (beach), grass and rock; two texture scales hide tiling seen from the air
float3 n = normalize(Nw);
float h = WP.z * 0.01;
float slope = 1.0 - saturate(n.z);
float macro = sin(WP.x * 0.00031 + 1.7) * sin(WP.y * 0.00027 + 0.4) * 0.5 + 0.5;
float wSand = saturate((5.0 - h) / 4.0);
float wRock = saturate((slope - 0.18) / 0.16) * (1.0 - wSand);
float wGrass = saturate(1.0 - wSand - wRock);
float2 uvA = WP.xy / 1500.0, uvB = WP.xy / 5200.0 + 0.37;
float3 g = lerp(Grass.Sample(GrassSampler, uvA).rgb, Grass.Sample(GrassSampler, uvB).rgb, 0.45);
float3 r = lerp(Rock.Sample(RockSampler, uvA * 0.4).rgb, Rock.Sample(RockSampler, uvB * 0.6).rgb, 0.5);
float3 s = Sand.Sample(SandSampler, WP.xy / 3000.0).rgb;
g *= lerp(0.8, 1.15, macro);
float3 c = g * wGrass + r * wRock + s * wSand;
// thin wet band at the waterline
c *= lerp(1.0, 0.62, saturate(1.0 - abs(h - 0.6) / 1.2));
return c;
"""


def build_terrain():
    grass, gn, ga = tex_set("aerial_grass_rock")
    rock, rn, ra = tex_set("aerial_rocks_02")
    sand, sn, sa = tex_set("aerial_beach_01")
    mat = fresh("M_Terrain", unreal.Material, unreal.MaterialFactoryNew())
    g = Graph(mat)
    wp = g.node(unreal.MaterialExpressionWorldPosition)
    nw = g.node(unreal.MaterialExpressionVertexNormalWS)
    to = lambda t: g.node(unreal.MaterialExpressionTextureObject, texture=t)
    c = g.custom(TERRAIN, [("WP", (wp, "")), ("Nw", (nw, "")), ("Grass", (to(grass), "")), ("Rock", (to(rock), "")), ("Sand", (to(sand), ""))], F3, "TerrainColour")
    MEL.connect_material_property(c, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = g.node(unreal.MaterialExpressionConstant, r=0.9)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


# ---------------------------------------------------------------- sea
SEA_N = r"""
// sum of travelling waves in world space; returns a world-space normal
float2 p = WP.xy * 0.01;
float2 d = 0;
const float2 Dir[6] = { float2(1, 0.2), float2(0.6, 0.8), float2(-0.3, 1), float2(0.9, -0.5), float2(-0.8, 0.4), float2(0.2, -1) };
const float Len[6] = { 38, 23, 15, 9, 5.5, 3.1 };
for (int i = 0; i < 6; ++i)
{
    float2 dd = normalize(Dir[i]);
    float k = 6.2831853 / Len[i];
    float ph = dot(dd, p) * k - Time * sqrt(9.81 * k);
    d += dd * cos(ph) * k * (0.012 * Len[i] / (1.0 + i * 0.35));
}
return normalize(float3(-d.x, -d.y, 1.0));
"""


def build_sea():
    mat = fresh("M_Sea", unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("tangent_space_normal", False)
    g = Graph(mat)
    wp = g.node(unreal.MaterialExpressionWorldPosition)
    tm = g.node(unreal.MaterialExpressionTime)
    n = g.custom(SEA_N, [("WP", (wp, "")), ("Time", (tm, ""))], F3, "SeaNormal")
    MEL.connect_material_property(n, "", unreal.MaterialProperty.MP_NORMAL)
    col = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.008, 0.045, 0.07, 1))
    MEL.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = g.node(unreal.MaterialExpressionConstant, r=0.06)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = g.node(unreal.MaterialExpressionConstant, r=0.5)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_paint():
    mat = fresh("M_Paint", unreal.Material, unreal.MaterialFactoryNew())
    g = Graph(mat)
    vc = g.node(unreal.MaterialExpressionVertexColor)
    MEL.connect_material_property(vc, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = g.node(unreal.MaterialExpressionConstant, r=0.32)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_canopy():
    mat = fresh("M_Canopy", unreal.Material, unreal.MaterialFactoryNew())
    g = Graph(mat)
    col = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.02, 0.035, 0.05, 1))
    MEL.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = g.node(unreal.MaterialExpressionConstant, r=0.04)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    met = g.node(unreal.MaterialExpressionConstant, r=0.6)
    MEL.connect_material_property(met, "", unreal.MaterialProperty.MP_METALLIC)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_trail():
    mat = fresh("M_Trail", unreal.Material, unreal.MaterialFactoryNew())
    set_flags(mat, blend_mode=unreal.BlendMode.BLEND_TRANSLUCENT, shading_model=unreal.MaterialShadingModel.MSM_UNLIT, two_sided=True)
    g = Graph(mat)
    vc = g.node(unreal.MaterialExpressionVertexColor)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    op = g.custom("float e = 1.0 - abs(UV.x * 2.0 - 1.0); return saturate(A * 0.42 * e * e);", [("A", (vc, "A")), ("UV", (uv, ""))], F1, "TrailOpacity")
    MEL.connect_material_property(op, "", unreal.MaterialProperty.MP_OPACITY)
    em = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(2.2, 2.3, 2.4, 1))
    MEL.connect_material_property(em, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_ring():
    mat = fresh("M_Ring", unreal.Material, unreal.MaterialFactoryNew())
    g = Graph(mat)
    col = g.node(unreal.MaterialExpressionVectorParameter, parameter_name="Color", default_value=unreal.LinearColor(1, 0.6, 0.1, 1))
    em = g.node(unreal.MaterialExpressionScalarParameter, parameter_name="Emissive", default_value=20.0)
    mul = g.node(unreal.MaterialExpressionMultiply)
    MEL.connect_material_expressions(col, "", mul, "A")
    MEL.connect_material_expressions(em, "", mul, "B")
    MEL.connect_material_property(mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


# ---------------------------------------------------------------- UE volumetric cloud material (route A)
def png_bytes(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def default_volume_texture():
    # a 4x4x4 placeholder volume texture: Volume texture parameters need a default of the right type
    full = f"{TEX}/VT_Placeholder"
    if EAL.does_asset_exist(full):
        return unreal.load_asset(full)
    tmp = os.path.join(PROJ, "Saved", "vt_placeholder_src.png")
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    w, h = 16, 4      # four 4x4 tiles side by side
    open(tmp, "wb").write(png_bytes(w, h, [0, 0, 0, 255] * (w * h)))
    tex = import_texture(tmp, "arm")
    if tex is None:
        return None
    tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    EAL.save_loaded_asset(tex)
    try:
        vt = AT.create_asset("VT_Placeholder", TEX, unreal.VolumeTexture, unreal.VolumeTextureFactory())
        vt.set_editor_property("source2d_tile_size_x", 4)
        vt.set_editor_property("source2d_tile_size_y", 4)
        vt.set_editor_property("source2d_texture", tex)   # rebuilds the volume from the 2D tiles (PostEditChange)
        EAL.save_loaded_asset(vt)
        return vt
    except Exception as e:  # noqa
        log("volume texture factory", e)
        return None


UE_CLOUD = r"""
// the same modelling NVDF and up-rez as the custom renderer, sampled per cloud sample by UE's ray marcher
float3 uvw = (WP - DomainMin.xyz) / DomainSize.xyz;
if (any(uvw < 0.0) || any(uvw > 1.0)) return float4(0, 0, 0, 0);
float4 m = Model.SampleLevel(ModelSampler, uvw, 0);
float profile = m.r, type = m.g, ds = m.b;
if (profile <= 0.0) return float4(0, 0, 0, 0);
float3 pm = WP * 0.01;
float4 nz = Noise.SampleLevel(NoiseSampler, pm * 0.01, 0);
float wispy = lerp(nz.r, nz.g, profile);
float billowy = lerp(nz.b * 0.3, nz.a * 0.3, pow(profile, 0.25));
float comp = lerp(wispy, billowy, type);
float d = saturate((profile - comp) / max(1.0 - comp, 1e-4));
float pds = pow(saturate(ds), 4.0);
d = pow(d * pds, lerp(0.3, 0.6, max(1e-4, pds)));
return float4(d, profile, 0, 0);
"""


def build_uecloud():
    vt = default_volume_texture()
    log("placeholder volume texture", vt)
    mat = fresh("M_UECloud", unreal.Material, unreal.MaterialFactoryNew())
    set_flags(mat, material_domain=unreal.MaterialDomain.MD_VOLUME, blend_mode=unreal.BlendMode.BLEND_ADDITIVE, used_with_volumetric_cloud=True)
    g = Graph(mat)
    wp = g.node(unreal.MaterialExpressionWorldPosition)
    tp = lambda n: g.node(unreal.MaterialExpressionTextureObjectParameter, parameter_name=n, texture=vt) if vt else None
    mdl, noi = tp("Model"), tp("Noise")
    dmin = g.node(unreal.MaterialExpressionVectorParameter, parameter_name="DomainMin", default_value=unreal.LinearColor(-204800, -204800, 20000, 0))
    dsize = g.node(unreal.MaterialExpressionVectorParameter, parameter_name="DomainSize", default_value=unreal.LinearColor(409600, 409600, 51200, 0))
    ins = [("WP", (wp, "")), ("DomainMin", (dmin, "")), ("DomainSize", (dsize, ""))]
    if mdl:
        ins += [("Model", (mdl, "")), ("Noise", (noi, ""))]
    c = g.custom(UE_CLOUD, ins, F4, "VoxelCloudDensity")
    ext = g.custom("return float3(1, 1, 1) * D.x * 0.05;   // UE takes extinction per metre", [("D", (c, ""))], F3, "Extinction")
    MEL.connect_material_property(ext, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    alb = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.95, 0.95, 0.95, 1))
    MEL.connect_material_property(alb, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # same phase function and a multiple-scattering approximation as the custom renderer; without this node
    # UE marches one scattering octave and back-lit cloud renders flat and dark
    adv = g.node(unreal.MaterialExpressionVolumetricAdvancedMaterialOutput)
    for k, v in [("const_phase_g", 0.6), ("const_phase_g2", -0.2), ("const_phase_blend", 0.3),
                 ("multi_scattering_approximation_octave_count", 2), ("const_multi_scattering_contribution", 0.5),
                 ("const_multi_scattering_occlusion", 0.5), ("const_multi_scattering_eccentricity", 0.5), ("ground_contribution", True)]:
        try:
            adv.set_editor_property(k, v)
        except Exception as e:
            log("advanced output property", k, e)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


# ---------------------------------------------------------------- sounds
def import_audio():
    d = os.path.join(SRC, "audio")
    if not os.path.isdir(d):
        log("no audio yet")
        return
    for f in sorted(os.listdir(d)):
        if not f.lower().endswith(".wav"):
            continue
        name = os.path.splitext(f)[0]
        if EAL.does_asset_exist(f"{AUD}/{name}"):
            continue
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", os.path.join(d, f))
        t.set_editor_property("destination_path", AUD)
        t.set_editor_property("automated", True)
        t.set_editor_property("save", False)
        AT.import_asset_tasks([t])
        snd = unreal.load_asset(f"{AUD}/{name}")
        if snd and name in ("S_Engine", "S_Wind"):
            snd.set_editor_property("looping", True)
        if snd:
            EAL.save_loaded_asset(snd)
        log("audio", name, bool(snd))


def build_map():
    path = "/Game/Maps/Skies"
    if EAL.does_asset_exist(path):
        log("map exists")
        return
    world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    log("map saved", unreal.EditorLoadingAndSavingUtils.save_map(world, path))


def main():
    for fn in (build_terrain, build_sea, build_paint, build_canopy, build_trail, build_ring, build_uecloud):
        try:
            build(fn)
        except Exception as e:  # noqa
            log("FAILED", fn.__name__, e)
    import_audio()
    build_map()
    EAL.save_directory(ROOT, only_if_is_dirty=False, recursive=True)
    log("done")


main()
