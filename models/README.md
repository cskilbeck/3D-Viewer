# Test models

A small set of files covering every kind of file and feature the viewer handles, about 1.3 MB in all. Each one is there to test something specific, and most are simple shapes so it's obvious when something's wrong.

## Checking them

```
step_viewer --check models            # load everything, compare with expected.json
step_viewer --check models --update   # rewrite expected.json from what loads now
```

The check runs without a window, takes about a second, and exits with 1 if anything doesn't match `expected.json`: whether it loads at all, the number of parts, triangles, materials and textures, whether it's shaded realistically, and its size in millimeters (to 0.01mm). New files and missing files count as failures too.

`expected.json` was made with `--update` after looking at every file in the viewer. When you change something on purpose, look at the affected files again before updating it.

## Making them

| Folder | Made by |
|---|---|
| `cad/` | `tools/make_cad_models.cpp` (OpenCascade): `cmake --build <build dir> --target make_cad_models`, then run it with `models/cad` |
| `gltf/` `mesh/` `errors/` | `tools/make_mesh_models.py` (needs Pillow): `python tools/make_mesh_models.py` |
| `samples/` | copied from Assimp's test models (`test/models` in the Assimp source, BSD license, see `samples/LICENSE-assimp.txt`) |

Unless it says otherwise, the "box" is 10 x 20 x 30 mm (X x Y x Z) and everything should come out Z up, the right size, and the right way round.

## CAD

| File | Tests | What you should see |
|---|---|---|
| `cad/assembly.step` | assembly tree, instances, part, face and transparent colors | red box with a yellow top, two blue cylinders (one standing, one lying down), and a sub assembly: green sphere under a see-through cyan plate. 47.5 x 52.5 x 30 |
| `cad/assembly.iges` | IGES | the same shapes and colors (IGES has no assembly structure, it's one part) |
| `cad/assembly.xbf` | OpenCascade XCAF document | the same as the STEP file |
| `cad/assembly.brep` | OpenCascade BREP | the same shapes, no names or colors |
| `cad/assembly.wrl` | VRML | the same shapes and colors, as meshes |
| `cad/inches.step` | STEP units | an orange box 1 x 2 x 3 inches, written in inches: 25.4 x 50.8 x 76.2 |
| `cad/far_from_origin.step` | precision a long way from the origin | a purple 10 mm cube a kilometer away in X and Y, with clean edges and no jitter |

## glTF

Everything's in meters (as glTF is) and Y up, so it gets scaled and turned.

| File | Tests | What you should see |
|---|---|---|
| `gltf/box.gltf` + `box.bin` | separate binary file | red box |
| `gltf/box_embedded.gltf` | base64 data in the `.gltf` | red box |
| `gltf/box.glb` | binary glTF | red box |
| `gltf/textured.glb` | PNG texture, texture orientation | 30 mm cube with the orientation texture on each face: red corner top left, green top right, blue bottom left, yellow bottom right, "TOP" at the top and an F the right way round |
| `gltf/textured_webp.glb` | WebP texture (`EXT_texture_webp`) | the same as `textured.glb` |
| `gltf/texture_transform.glb` | `KHR_texture_transform` | three squares: the texture as is, mirrored left to right, and rotated 90 degrees |
| `gltf/alpha_modes.glb` | alpha modes | three striped squares in front of a blue bar: opaque (alpha ignored), mask (the right half cut away cleanly), blend (fading out to the right) |
| `gltf/pbr_spheres.glb` | metallic / roughness | 3 x 3 gold spheres: metallic 0, 0.5, 1 left to right, roughness 0.1, 0.5, 1 bottom to top |
| `gltf/normal_map.glb` | normal map | a 40 mm metal plate with 4 x 4 round dimples |
| `gltf/emissive.glb` | emissive color and `KHR_materials_emissive_strength` | a glowing orange cube next to a plain red one |
| `gltf/hierarchy.glb` | node tree, one mesh used several times | five textured cubes: plain, turned 45 degrees, mirrored (F backwards), stretched (twice as tall, half as deep), and one nested three nodes deep |
| `gltf/far_from_origin.glb` | precision a long way from the origin | a red 10 mm cube a kilometer away |

## Other mesh formats

| File | Tests | What you should see |
|---|---|---|
| `mesh/box_ascii.stl` | ASCII STL | grey box |
| `mesh/box_binary.stl` | binary STL | grey box |
| `mesh/ünïcødé_名前.stl` | a file name which isn't ASCII | grey box |
| `mesh/textured_box.obj` + `.mtl` + `orientation.png` | OBJ with a material and texture (Y up) | the box with the orientation texture on each face |
| `mesh/missing_texture.obj` + `.mtl` | a texture file which isn't there | the box in its material's red, no texture, still loads |
| `mesh/cylinder_no_normals.obj` | a mesh with no normals | r5 x 20 mm cylinder: smooth sides, sharp edges round the ends |
| `mesh/box_ascii.ply` | ASCII PLY (Y up) | grey box |
| `mesh/box_binary.ply` | binary PLY (Y up) | grey box |
| `mesh/box.off` | OFF | grey box |
| `mesh/box_z_up_cm.dae` | Collada in centimeters with Z up | green box, the same size and way up as the others |
| `mesh/box.x3d` | X3D (meters, Y up) | orange box |
| `mesh/box.amf` | AMF | grey box |
| `mesh/box.3mf` | 3MF | grey box |

## Samples

From Assimp's tests, for formats which aren't worth generating. The sizes are whatever the files say (most have no units, so they're taken as millimeters).

| File | Tests |
|---|---|
| `samples/box.fbx` | binary FBX |
| `samples/cubes_with_mirroring_and_pivot.fbx` | FBX with mirrored instances and pivots |
| `samples/maxPbrMaterial_metalRough.fbx` | FBX with a PBR material (3ds Max) |
| `samples/draco/2CylinderEngine.gltf` + `.bin` | Draco compressed glTF (the Khronos 2CylinderEngine sample, © Okino, CC BY 4.0) |
| `samples/cubes_with_alpha.3DS` | 3D Studio, with transparency |
| `samples/test_cube_text.x`, `test_cube_binary.x` | DirectX, text and binary |
| `samples/boxuv.lwo` + `boxuv.png` | LightWave with a texture |
| `samples/ThreeCubesGreen.ASE` | 3ds Max ASE |
| `samples/sample_subdiv.ac` | AC3D (subdivision surface) |
| `samples/twospheres_withmats.ms3d` | Milkshape 3D |
| `samples/WusonBlitz.b3d` | Blitz3D |
| `samples/SimpleCube.md5mesh` | Doom 3 MD5 |
| `samples/triangle.smd` | Valve SMD |
| `samples/Example.ogex` | OpenGEX |
| `samples/cone.nff` | Neutral File Format |
| `samples/box.uc`, `box_a.3d`, `box_d.3d` | Unreal (any of the three files loads the model) |
| `samples/PinkEggFromLW.dxf` | DXF (3D faces) |

## Files which shouldn't load

They should give an error, not crash, hang or show an empty view.

| File | Tests |
|---|---|
| `errors/empty.stl`, `errors/empty.glb` | empty files |
| `errors/not_really.step`, `errors/not_really.obj` | files which aren't what their extension says |
| `errors/truncated.glb` | half a file |
