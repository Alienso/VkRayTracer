# Converts an Amazon Lumberyard Bistro FBX (developer.nvidia.com/orca/amazon-lumberyard-bistro) to glTF.
#   blender -b --factory-startup --python tools/bistro/convert_bistro.py -- <Bistro_v5_2>/BistroExterior.fbx
#           resource/models/Bistro [emissive strength] [frame]
# The same for BistroInterior.fbx and BistroInterior_Wine.fbx; they share the Textures folder (about 1.4 GB).
# Other NVIDIA ORCA scenes with the same texture convention work too, e.g. Beeple's Zero-Day
# (developer.nvidia.com/orca/beeple-zero-day): <ZeroDay_v1>/MEASURE_ONE/MEASURE_ONE.fbx resource/models/ZeroDay 1 1.
# Its textures are in "tex" next to the FBX, some glowing materials use "Emissive2" textures, and it is animated:
# the app shows still scenes, so one frame of the animation is converted (frame, default the file's first).
# Tested with Blender 5.2 (its Python has numpy).
# Geometry: Blender's FBX importer + glTF exporter (no images). Materials are then written directly into the
# .gltf from the Bistro texture convention (README): BaseColor (alpha = cut-out), Specular (R occlusion,
# G roughness, B metallic = glTF's layout), Normal (BC5, mostly DirectX), Emissive. Textures stay compressed DDS,
# referenced through MSFT_texture_dds; normal maps get their green channel flipped where needed (exactly, in the
# blocks). Emissive textures become PNG so the app can still average them for light sampling.
# Guesses (Falcor sets these in its scene files): every material named "...glass..." is clear glass, except the
# Exterior's windows (MASTER_Glass*: opaque and dark, its buildings have no insides); leaves,
# flowers, curtains and awnings let light through (KHR_materials_diffuse_transmission); the
# Wine scene's liquids and glasses get the absorption from BistroInterior_Wine.pyscene. The Interior's lamps become
# KHR_lights_punctual spots (see "Lights" below). The
# directional light and cameras are dropped (the app has its own sun and sky).
import bpy, sys, os, json, shutil, struct, zlib, math, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import bistro_dds as dds

args = sys.argv[sys.argv.index('--') + 1:]
fbx_path = os.path.abspath(args[0])
out_dir = os.path.abspath(args[1])
emissive_strength = float(args[2]) if len(args) > 2 else 1.0
frame = int(args[3]) if len(args) > 3 else None
src_textures = next((os.path.join(os.path.dirname(fbx_path), d) for d in ('Textures', 'tex')
                     if os.path.isdir(os.path.join(os.path.dirname(fbx_path), d))), None)
assert src_textures, 'no Textures or tex folder next to ' + fbx_path
out_textures = os.path.join(out_dir, 'Textures')
os.makedirs(out_textures, exist_ok=True)
name = os.path.splitext(os.path.basename(fbx_path))[0]
wine = name == 'BistroInterior_Wine'
exterior = name == 'BistroExterior'

# ------------------------------------------------------------------------------------------------
# Geometry
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_path)
if frame is not None:
    bpy.context.scene.frame_set(frame)  # animated objects move there; the export below takes this pose
for o in list(bpy.data.objects):
    # Cameras and the Exterior's directional light (the app has its own sun) go; point and spot lights become
    # KHR_lights_punctual. Empties stay: some hold the parent transform (cm -> m).
    if o.type == 'CAMERA' or (o.type == 'LIGHT' and o.data.type not in ('POINT', 'SPOT')):
        bpy.data.objects.remove(o)
for m in bpy.data.meshes:
    while len(m.uv_layers) > 1:          # the second set is a lightmap layout, unused
        m.uv_layers.remove(m.uv_layers[1])

def texture_files(material):
    files = {}
    if material.node_tree:
        for n in material.node_tree.nodes:
            if n.type == 'TEX_IMAGE' and n.image:
                base = os.path.basename(bpy.path.abspath(n.image.filepath))
                kind = os.path.splitext(base)[0].rsplit('_', 1)[-1].lower()
                if kind == 'emissive2':
                    kind = 'emissive'  # Zero-Day: a second emissive texture, the material's only one
                files[kind] = base
    return files

material_info = {m.name: texture_files(m) for m in bpy.data.materials}

gltf_path = os.path.join(out_dir, name + '.gltf')
bpy.ops.export_scene.gltf(filepath=gltf_path, export_format='GLTF_SEPARATE', export_image_format='NONE',
                          export_tangents=True, export_lights=True, export_cameras=False, export_extras=False,
                          export_yup=True, export_materials='EXPORT', export_animations=False)

# ------------------------------------------------------------------------------------------------
# Textures
def find_texture(base):
    for candidate in (base, os.path.splitext(base)[0] + '.dds', os.path.splitext(base)[0] + '.tga',
                      os.path.splitext(base)[0] + '.TGA'):
        p = os.path.join(src_textures, candidate)
        if os.path.exists(p):
            return p
    return None

def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def decode_bc1_rgb(path):
    info = dds.read_header(path)
    blocks = dds.level0_blocks(path, info)
    if info['format'] == 'DXT5':
        blocks = blocks[..., 8:]
    c0 = blocks[..., 0].astype(np.uint32) | (blocks[..., 1].astype(np.uint32) << 8)
    c1 = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 8)
    def rgb565(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.int32)
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1)[..., None] | (info['format'] == 'DXT5')
    palette = np.stack([p0, p1, np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2),
                        np.where(four, (p0 + 2 * p1) // 3, 0)], -2)            # (bh, bw, 4, 3)
    bits = (blocks[..., 4].astype(np.uint32) | (blocks[..., 5].astype(np.uint32) << 8) |
            (blocks[..., 6].astype(np.uint32) << 16) | (blocks[..., 7].astype(np.uint32) << 24))
    idx = np.stack([(bits >> (2 * t)) & 3 for t in range(16)], -1).astype(np.int64)   # (bh, bw, 16)
    texels = np.take_along_axis(palette, idx[..., None].repeat(3, -1), -2)         # (bh, bw, 16, 3)
    bh, bw = blocks.shape[:2]
    image = texels.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)
    return image[:info['height'], :info['width']].astype(np.uint8)

images, image_index = [], {}
def image_for(kind, base):
    """Index of the glTF image for a Bistro texture, converting / copying it on first use; None if missing."""
    src = find_texture(base)
    if src is None:
        print('MISSING texture', base)
        return None
    key = (kind, os.path.basename(src))
    if key in image_index:
        return image_index[key]
    stem, ext = os.path.splitext(os.path.basename(src))
    if ext.lower() == '.dds' and kind == 'emissive':
        out_name = stem + '.png'
        if not os.path.exists(os.path.join(out_textures, out_name)):
            write_png(os.path.join(out_textures, out_name), decode_bc1_rgb(src))
    elif ext.lower() == '.dds' and kind == 'normal':
        # The README says DirectX (Y down the image), true for most; some maps are already Y up (curtains,
        # plants, frosted glass, ...): flip unless the map itself clearly says Y up.
        out_name = stem + '_GL.dds'
        if not os.path.exists(os.path.join(out_textures, out_name)):
            score = dds.y_up_score(src)
            if score > 0.15:
                print('normal map already Y up: %s (%.2f)' % (stem, score))
            dds.write_bc5(src, os.path.join(out_textures, out_name), flip_green=score <= 0.15)
    else:
        out_name = os.path.basename(src)
        if not os.path.exists(os.path.join(out_textures, out_name)):
            shutil.copyfile(src, os.path.join(out_textures, out_name))
    image_index[key] = len(images)
    images.append({'uri': 'Textures/' + out_name, 'name': stem,
                   'mimeType': 'image/vnd-ms.dds' if out_name.endswith('.dds') else 'image/png'})
    return image_index[key]

def texture_ref(gltf, kind, base):
    i = image_for(kind, base)
    if i is None:
        return None
    if images[i]['uri'].endswith('.dds'):
        texture = {'sampler': 0, 'extensions': {'MSFT_texture_dds': {'source': i}}}
    else:
        texture = {'sampler': 0, 'source': i}
    gltf['textures'].append(texture)
    return {'index': len(gltf['textures']) - 1}

alpha_cache = {}
def is_cutout(base):
    """Alpha is a cut-out mask when part (not all, not none) of the texture is below 0.5. Some textures have
    alpha 0 everywhere (table cloth, bottle labels): unused, they must stay visible."""
    src = find_texture(base)
    if src is None or not src.lower().endswith('.dds'):
        return False
    if src not in alpha_cache:
        a = dds.bc3_alpha(src)
        alpha_cache[src] = a is not None and 0.001 < float((a < 128).mean()) < 0.999
    return alpha_cache[src]

# ------------------------------------------------------------------------------------------------
# Materials
with open(gltf_path) as f:
    gltf = json.load(f)
gltf['samplers'] = [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}]
gltf['textures'] = []

# BistroInterior_Wine.pyscene: liquids and glass (absorption per meter, the file's units after import)
wine_volumes = {
    'TransparentGlass': dict(ior=1.55, roughness=0.0),
    'TransparentGlassWine': dict(ior=1.55, roughness=0.0, absorption=(102.68063, 168.015, 246.80438)),
    'Water': dict(ior=1.33, roughness=0.0),
    'Ice': dict(ior=1.31, roughness=0.1),
    'White_Wine': dict(ior=1.33, roughness=0.0, absorption=(12.28758, 16.51818, 20.30273)),
    'Red_Wine': dict(ior=1.33, roughness=0.0, absorption=(117.13133, 251.91133, 294.33867)),
    'Beer': dict(ior=1.33, roughness=0.0, absorption=(11.78552, 25.45862, 58.37241)),
}

stats = dict(materials=0, cutout=0, glass=0, windows=0, emissive=0, volumes=0, translucent=0, lights=0)
for material in gltf.get('materials', []):
    mname = material['name']
    files = material_info.get(mname, {})
    new = {'name': mname, 'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, 1], 'metallicFactor': 1.0,
                                                    'roughnessFactor': 1.0}}
    pbr = new['pbrMetallicRoughness']
    extensions = {}
    if 'basecolor' in files:
        ref = texture_ref(gltf, 'basecolor', files['basecolor'])
        if ref:
            pbr['baseColorTexture'] = ref
            if is_cutout(files['basecolor']):
                new['alphaMode'] = 'MASK'
                new['alphaCutoff'] = 0.5
                stats['cutout'] += 1
    if 'specular' in files:
        ref = texture_ref(gltf, 'specular', files['specular'])
        if ref:
            pbr['metallicRoughnessTexture'] = ref
    else:
        pbr['metallicFactor'] = 0.0
    if 'normal' in files:
        ref = texture_ref(gltf, 'normal', files['normal'])
        if ref:
            new['normalTexture'] = ref
    if 'emissive' in files:
        ref = texture_ref(gltf, 'emissive', files['emissive'])
        if ref:
            new['emissiveTexture'] = ref
            new['emissiveFactor'] = [1.0, 1.0, 1.0]
            if emissive_strength != 1.0:
                extensions['KHR_materials_emissive_strength'] = {'emissiveStrength': emissive_strength}
            stats['emissive'] += 1

    # Glass: Falcor marks it per scene file; here every material named like glass lets light through
    base_name = mname.split('.')[0]
    if wine and base_name in wine_volumes:
        v = wine_volumes[base_name]
        pbr['metallicFactor'] = 0.0
        pbr['roughnessFactor'] = v['roughness']
        pbr.pop('metallicRoughnessTexture', None)
        pbr.pop('baseColorTexture', None)
        extensions['KHR_materials_transmission'] = {'transmissionFactor': 1.0}
        extensions['KHR_materials_ior'] = {'ior': v['ior']}
        volume = {'thicknessFactor': 1.0}
        if 'absorption' in v:
            # color after a distance d: exp(-sigma * d); d = 1 / largest sigma keeps the color readable
            d = 1.0 / max(v['absorption'])
            volume['attenuationDistance'] = d
            volume['attenuationColor'] = [math.exp(-s * d) for s in v['absorption']]
        extensions['KHR_materials_volume'] = volume
        new.pop('alphaMode', None)
        stats['volumes'] += 1
    elif exterior and base_name.startswith('MASTER_Glass'):
        # The Exterior's windows and doors: its buildings are hollow shells, so clear glass would show their
        # empty insides and the sky. Kept opaque like the original (and Falcor): the dark base color is the room
        # behind, and the smooth surface shows the street reflected in it.
        pbr['metallicFactor'] = 0.0
        pbr['roughnessFactor'] = 0.0
        pbr.pop('metallicRoughnessTexture', None)
        if 'baseColorTexture' not in pbr:
            pbr['baseColorFactor'] = [0.03, 0.03, 0.03, 1.0]
        stats['windows'] += 1
        print('window:', mname)
    elif 'glass' in mname.lower() and 'glasses' not in mname.lower() and new.get('alphaMode') != 'MASK':
        # The base colors are dark (0 - 0.18: made for see-through blending in a rasterizer), and as a
        # transmission tint they would make the glass black: clear glass instead. Their roughness textures
        # are mostly a flat 0.74; only frosted glass keeps it.
        pbr['metallicFactor'] = 0.0
        pbr.pop('baseColorTexture', None)
        if not any(k in mname.lower() for k in ('frost', 'frozen')):
            pbr.pop('metallicRoughnessTexture', None)
            pbr['roughnessFactor'] = 0.0
        extensions['KHR_materials_transmission'] = {'transmissionFactor': 1.0}
        stats['glass'] += 1
        print('glass:', mname)

    # Thin things the sun shines through: leaves and flowers (about half of what isn't reflected comes out
    # the back, tinted like the leaf), curtains and awnings (less). Their own color tints the light.
    translucency = 0.0
    if re.search(r'Foliage_(Bux_Hedges|Flowers|Ivy_leaf|Leaves|Linde_Tree_Large_\w+_Leaves|Paris_Flowers)|Plants_plants', mname):
        translucency = 0.5
    elif re.search(r'curtain|awnings?_(\w+_)?fabric', mname, re.I):
        translucency = 0.3
    if translucency > 0.0:
        dt = {'diffuseTransmissionFactor': translucency}
        if 'baseColorTexture' in pbr:
            dt['diffuseTransmissionColorTexture'] = dict(pbr['baseColorTexture'])
        extensions['KHR_materials_diffuse_transmission'] = dt
        stats['translucent'] += 1
        print('translucent %.1f: %s' % (translucency, mname))
    if extensions:
        new['extensions'] = extensions
    material.clear()
    material.update(new)
    stats['materials'] += 1

# Lights (only the Interior has any: 4 point lights, one at the bottom opening of each pendant lamp).
# - Blender's exporter turns their 400 W (its reading of the FBX) into 21740 cd, as if all of it were visible light:
#   thousands of times brighter than the app's sun and sky. Calibrated by eye instead: 35 makes the lamps light
#   the room about as much as the daylight through the windows (the app's sun gives pi lux, not 100 000).
# - Inside or touching the lamp, a point light makes the lamp's own surfaces centimeters away extremely bright, which
#   paths find only rarely: speckles all over the room. As the shades send the light down anyway, each becomes a spot
#   pointing down (the exported nodes already do), 3 cm lower, with a cone of almost a half sphere: the shade stays
#   outside it, and the ceiling isn't lit from below.
LAMP_INTENSITY = 35.0
LAMP_DROP = 0.03
for light in gltf.get('extensions', {}).get('KHR_lights_punctual', {}).get('lights', []):
    if light['type'] == 'point':
        light['intensity'] = LAMP_INTENSITY
        light['type'] = 'spot'
        light['spot'] = {'innerConeAngle': 1.0, 'outerConeAngle': 1.45}
for node in gltf['nodes']:
    if 'KHR_lights_punctual' in node.get('extensions', {}):
        assert gltf['nodes'].index(node) in gltf['scenes'][0]['nodes'], 'light node with a parent: translation is not world'
        node['translation'][1] -= LAMP_DROP
        gltf['extensions']['KHR_lights_punctual']['lights'][node['extensions']['KHR_lights_punctual']['light']]['name'] = node['name']
        stats['lights'] += 1

gltf['images'] = images
used = set(gltf.get('extensionsUsed', []))
used.update(['MSFT_texture_dds', 'KHR_materials_transmission', 'KHR_materials_ior', 'KHR_materials_volume',
             'KHR_materials_emissive_strength', 'KHR_materials_diffuse_transmission'])
gltf['extensionsUsed'] = sorted(used)
gltf['extensionsRequired'] = ['MSFT_texture_dds']
with open(gltf_path, 'w') as f:
    json.dump(gltf, f, indent=1)

tris = 0
for mesh in gltf['meshes']:
    for p in mesh['primitives']:
        tris += gltf['accessors'][p['indices']]['count'] // 3
print('CONVERTED', name, stats, 'images', len(images), 'meshes', len(gltf['meshes']), 'nodes', len(gltf['nodes']),
      'triangles', tris)
