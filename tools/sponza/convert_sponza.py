# Converts Intel's Sponza 2022 (intel.com, Graphics Research Samples: the main scene and its add-on packages, each a
# glTF with 4K PNG textures) to glTF with block-compressed DDS textures (MSFT_texture_dds).
#   blender -b --factory-startup --python tools/sponza/convert_sponza.py -- <main_sponza>/NewSponza_Main_glTF_003.gltf
#           resource/models/NewSponza/Main
# and the same for each package (pkg_a_curtains/NewSponza_Curtains_glTF.gltf -> .../Curtains, ...). The packages
# share the main scene's coordinates: add them all with "Original size" and they fit together.
# Why: 72 textures of 4096 x 4096 in the main scene alone are 6 GB as 8-bit RGBA with mips, too much for an 8 GB GPU
# (and slow to decode at every start). Compressed: base colors BC1 (BC3 where the material uses alpha), metallic-
# roughness BC1, normal maps BC5 (X and Y; the app rebuilds Z), with all mip levels: about 1/6 of that.
# Geometry stays as it is (the .bin is copied). Blender is only used to read the PNGs (its Python has numpy).
# Also: lights with intensity 0 (all of the main scene's 24) are dropped; the app would still trace shadow rays to them.
import bpy, sys, os, json, shutil, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bistro'))
import numpy as np
import bc_encode as bc
import bistro_dds as dds

args = sys.argv[sys.argv.index('--') + 1:]
src_gltf = os.path.abspath(args[0])
out_dir = os.path.abspath(args[1])
src_dir = os.path.dirname(src_gltf)
os.makedirs(os.path.join(out_dir, 'textures'), exist_ok=True)

with open(src_gltf, encoding='utf-8') as f:
    gltf = json.load(f)

# What each image is used for decides its format and color space
roles = {}
def use(texture_ref, role):
    if texture_ref is not None:
        image = gltf['textures'][texture_ref['index']]['source']
        roles.setdefault(image, set()).add(role)
for m in gltf.get('materials', []):
    pbr = m.get('pbrMetallicRoughness', {})
    blended = m.get('alphaMode', 'OPAQUE') != 'OPAQUE'
    use(pbr.get('baseColorTexture'), 'color_alpha' if blended else 'color')
    use(pbr.get('metallicRoughnessTexture'), 'data')
    use(m.get('normalTexture'), 'normal')
    use(m.get('occlusionTexture'), 'data')
    use(m.get('emissiveTexture'), 'color')

def load_png(path):
    image = bpy.data.images.load(path, check_existing=False)
    image.colorspace_settings.name = 'Non-Color'  # the stored values, no conversion
    w, h = image.size
    pixels = np.empty(w * h * 4, np.float32)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)
    return np.ascontiguousarray(pixels.reshape(h, w, 4)[::-1]) * 255.0  # Blender's rows go bottom-up

total_in = total_out = 0
start = time.time()
for i, image in enumerate(gltf['images']):
    role = roles.get(i, {'color'})
    if len(role) > 1:
        print('WARNING image %d used as %s, converted as %s' % (i, role, sorted(role)[0]))
    role = sorted(role)[0]
    uri = image['uri'].replace('%20', ' ')
    src = os.path.normpath(os.path.join(src_dir, uri))
    stem = os.path.splitext(os.path.basename(src))[0]
    out_name = 'textures/' + stem + '.dds'
    out_path = os.path.join(out_dir, out_name)
    fmt = {'color': 'DXT1', 'color_alpha': 'DXT5', 'data': 'DXT1', 'normal': 'ATI2'}[role]
    if not os.path.exists(out_path):
        pixels = load_png(src)
        h, w = pixels.shape[:2]
        levels = bc.mip_chain(pixels, srgb=role.startswith('color'))
        if role == 'normal':
            # glTF wants Y up the image (OpenGL), but some of these are DirectX (Y down): the same no-curl test as
            # for Bistro (bistro_dds.y_up_score) on the full-size level, and green flipped where clearly Y down.
            x, y = pixels[..., 0] / 127.5 - 1.0, pixels[..., 1] / 127.5 - 1.0
            dx_dyup = -(x[1:, :-1] - x[:-1, :-1])
            dy_dx = y[:-1, 1:] - y[:-1, :-1]
            score = float((dx_dyup * dy_dx).mean() / (np.sqrt((dx_dyup ** 2).mean() * (dy_dx ** 2).mean()) + 1e-12))
            if score < -0.15:
                print('normal map is DirectX (Y down), green flipped: %s (%.2f)' % (stem, score))
                pixels[..., 1] = 255.0 - pixels[..., 1]
                levels = bc.mip_chain(pixels, srgb=False)
            # Averaged normals get shorter; X and Y back to unit length with the rebuilt Z keeps the slope's direction
            fixed = []
            for level in levels:
                n = level[..., :3] / 127.5 - 1.0
                n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
                fixed.append(np.concatenate([(n + 1.0) * 127.5, level[..., 3:]], -1))
            levels = fixed
        bc.write_dds(out_path, w, h, fmt, [bc.encode(level, fmt) for level in levels])
        print('%-60s %s %dx%d %d levels  %.0fs' % (stem, fmt, w, h, len(levels), time.time() - start))
    total_in += os.path.getsize(src)
    total_out += os.path.getsize(out_path)
    if role == 'normal':
        score = dds.y_up_score(out_path)
        if score < -0.15:
            print('WARNING normal map still looks DirectX (Y down): %s (%.2f)' % (stem, score))
    image.clear()
    image.update({'uri': out_name, 'name': stem, 'mimeType': 'image/vnd-ms.dds'})

for texture in gltf['textures']:
    source = texture.pop('source')
    texture.setdefault('extensions', {})['MSFT_texture_dds'] = {'source': source}

# Lights that give no light
lights = gltf.get('extensions', {}).get('KHR_lights_punctual', {}).get('lights', [])
if lights:
    keep = [i for i, l in enumerate(lights) if l.get('intensity', 1.0) > 0.0]
    remap = {old: new for new, old in enumerate(keep)}
    gltf['extensions']['KHR_lights_punctual']['lights'] = [lights[i] for i in keep]
    for node in gltf['nodes']:
        ext = node.get('extensions', {}).get('KHR_lights_punctual')
        if ext is not None:
            if ext['light'] in remap:
                ext['light'] = remap[ext['light']]
            else:
                del node['extensions']['KHR_lights_punctual']
                if not node['extensions']:
                    del node['extensions']
    print('lights kept: %d of %d' % (len(keep), len(lights)))

for buffer in gltf['buffers']:
    uri = buffer['uri'].replace('%20', ' ')
    target = os.path.join(out_dir, os.path.basename(uri))
    if not os.path.exists(target) or os.path.getsize(target) != os.path.getsize(os.path.join(src_dir, uri)):
        shutil.copyfile(os.path.join(src_dir, uri), target)
    buffer['uri'] = os.path.basename(uri)

used = set(gltf.get('extensionsUsed', []))
used.add('MSFT_texture_dds')
gltf['extensionsUsed'] = sorted(used)
gltf['extensionsRequired'] = sorted(set(gltf.get('extensionsRequired', [])) | {'MSFT_texture_dds'})
name = os.path.splitext(os.path.basename(src_gltf))[0].replace('_glTF', '')
with open(os.path.join(out_dir, name + '.gltf'), 'w', encoding='utf-8') as f:
    json.dump(gltf, f, indent=1)
print('CONVERTED %s: %d images, PNG %.0f MB -> DDS %.0f MB (with mips), %.0fs' %
      (name, len(gltf['images']), total_in / 2**20, total_out / 2**20, time.time() - start))
