#pragma once

namespace blendnet::blender::detail {
// Scripts de l'application, jamais du Python reçu d'un pair réseau.
inline constexpr auto probeScript = R"PY(
import bpy, json, sys
sys.dont_write_bytecode = True
p = bpy.context.preferences.addons['cycles'].preferences
devices = []
try:
    p.refresh_devices()
    for d in p.devices:
        if d.type == 'CPU': devices.append({'name': d.name, 'backend': 'CPU'})
except Exception:
    pass
if not devices: devices.append({'name': 'CPU', 'backend': 'CPU'})
for kind in ('OPTIX', 'CUDA', 'HIP', 'ONEAPI', 'METAL'):
    try:
        p.compute_device_type = kind
        p.refresh_devices()
        for d in p.devices:
            if d.type == kind: devices.append({'name': d.name, 'backend': kind})
    except Exception:
        pass
eevee = ''
for engine in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE'):
    try:
        bpy.context.scene.render.engine = engine
        eevee = engine
        break
    except Exception:
        pass
limits = bpy.types.Scene.bl_rna.properties
print('BLENDNET_GPU:' + json.dumps({'version': '.'.join(map(str, bpy.app.version)), 'devices': devices, 'eeveeEngine': eevee,
    'frameBounds': [limits['frame_start'].hard_min, limits['frame_end'].hard_max, limits['frame_step'].hard_max]}), flush=True)
)PY";
inline constexpr auto configureRenderScript = R"PY(
import bpy, json, sys
sys.dont_write_bytecode = True
index = sys.argv.index('--blendnet-request')
request = json.loads(sys.argv[index + 1])
s = bpy.context.scene
s.frame_start = request['start']
s.frame_end = request['end']
s.frame_step = request['step']
if (s.frame_start, s.frame_end, s.frame_step) != (request['start'], request['end'], request['step']):
    raise ValueError('Frame range exceeds Blender limits')
s.render.use_file_extension = True
s.render.use_overwrite = True
s.render.use_placeholder = False
)PY";
inline constexpr auto referenceSceneScript = R"PY(
import bpy, sys, math
from mathutils import Vector
sys.dont_write_bytecode = True
args = sys.argv[sys.argv.index('--') + 1:]
path, purpose = args[0], args[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
s = bpy.context.scene
s.render.engine = 'CYCLES'
s.cycles.samples = 32 if purpose == 'benchmark' else 1
s.cycles.use_denoising = False
s.cycles.max_bounces = 4
s.render.resolution_x = s.render.resolution_y = 256 if purpose == 'benchmark' else 32
s.render.resolution_percentage = 100
s.render.image_settings.file_format = 'PNG'
s.frame_start, s.frame_end = 1, 3
bpy.ops.mesh.primitive_monkey_add(location=(0, 0, 0.8))
subject = bpy.context.object
subject.rotation_euler.z = 0
subject.keyframe_insert(data_path='rotation_euler', frame=1)
subject.rotation_euler.z = 0.4
subject.keyframe_insert(data_path='rotation_euler', frame=3)
bpy.ops.mesh.primitive_plane_add(size=20)
bpy.ops.object.light_add(type='AREA', location=(2, -3, 5))
bpy.context.object.data.energy = 600
bpy.context.object.data.size = 4
bpy.ops.object.camera_add(location=(3, -5, 3))
camera = bpy.context.object
camera.rotation_euler = (Vector((0, 0, 0.7)) - camera.location).to_track_quat('-Z', 'Y').to_euler()
s.camera = camera
s.world = bpy.data.worlds.new('BlendNet world')
s.world.color = (0.2, 0.2, 0.2)
bpy.ops.wm.save_as_mainfile(filepath=path)
)PY";
} // namespace blendnet::blender::detail
