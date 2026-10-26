"""Exercise the embedded inspection script without downloading Blender."""

import contextlib
import io
import json
import pathlib
import re
import sys
import types
import unittest


source = pathlib.Path(sys.argv.pop(1)).read_text()
inspection = re.search(r'constexpr auto inspection = R"PY\((.*?)\)PY";', source, re.S).group(1)


class Paint:
    def __init__(self, library_type="ESSENTIALS", identifier="brushes/essentials_brushes-gp_draw.blend/Brush/Pencil"):
        self.brush_asset_reference = types.SimpleNamespace(
            asset_library_type=library_type, relative_asset_identifier=identifier)


def scene(paint):
    return types.SimpleNamespace(
        render=types.SimpleNamespace(engine="CYCLES", resolution_x=32, resolution_y=32,
                                     resolution_percentage=100,
                                     image_settings=types.SimpleNamespace(file_format="PNG")),
        frame_start=1, frame_end=3, frame_step=1,
        tool_settings=types.SimpleNamespace(
            gpencil_paint=paint,
            bl_rna=types.SimpleNamespace(properties=[
                types.SimpleNamespace(type="POINTER", identifier="gpencil_paint")]))
    )


def inspect(paths, paints, linked=(), modifiers=()):
    scenes = [scene(paint) for paint in paints]
    bpy = types.ModuleType("bpy")
    bpy.context = types.SimpleNamespace(scene=scenes[0])
    bpy.app = types.SimpleNamespace(version=(5, 2, 2))
    bpy.types = types.SimpleNamespace(Paint=Paint)
    bpy.utils = types.SimpleNamespace(blend_paths=lambda **kwargs: paths)
    bpy.path = types.SimpleNamespace(abspath=lambda path, **kwargs: path)
    bpy.data = types.SimpleNamespace(
        scenes=scenes,
        libraries=[types.SimpleNamespace(filepath=path, parent=None) for path in linked],
        objects=[types.SimpleNamespace(modifiers=modifiers)])
    old = sys.modules.get("bpy")
    sys.modules["bpy"] = bpy
    try:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exec(inspection, {})
        return json.loads(output.getvalue().split("BLENDNET_PROJECT:", 1)[1])["dependencies"]
    finally:
        if old is None:
            del sys.modules["bpy"]
        else:
            sys.modules["bpy"] = old


class ProjectDependenciesTest(unittest.TestCase):
    # A stale Snap revision can be absent locally. Do not require its existence.
    brush = "/home/test/snap/blender/7803/5.2/datafiles/assets/brushes/essentials_brushes-gp_draw.blend"

    def test_essentials_weak_reference_is_not_a_render_dependency(self):
        texture = "/project/textures/color.png"
        self.assertEqual(inspect([self.brush, texture, self.brush], [Paint()]), [texture])

    def test_genuinely_linked_library_is_preserved_even_with_same_weak_reference(self):
        self.assertEqual(inspect([self.brush], [Paint()], linked=[self.brush]), [self.brush])

    def test_custom_brush_and_other_assets_are_preserved(self):
        other = "/project/datafiles/assets/brushes/custom.blend"
        self.assertEqual(inspect([self.brush, other], [Paint("CUSTOM")]),
                         sorted([self.brush, other]))
        self.assertEqual(inspect([other], [Paint()]), [other])

    def test_brush_references_in_other_scenes_are_considered(self):
        self.assertEqual(inspect([self.brush], [None, Paint()]), [])

    def test_blender_without_brush_asset_api_keeps_dependencies(self):
        paint = Paint()
        del paint.brush_asset_reference
        self.assertEqual(inspect([self.brush], [paint]), [self.brush])

    def test_simulation_caches_are_not_hidden_by_brush_filter(self):
        modifier = types.SimpleNamespace(
            domain_settings=types.SimpleNamespace(cache_directory="/project/fluid"),
            point_cache=types.SimpleNamespace(use_disk_cache=True, filepath="/project/particles"))
        self.assertEqual(inspect([self.brush], [Paint()], modifiers=[modifier]),
                         ["/project/fluid", "/project/particles"])


if __name__ == "__main__":
    unittest.main()
