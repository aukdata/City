# Distant building meshes

Generated from the 42 residential and commercial OBJ models in the adjacent directories by `scripts/build_city_lod.py`, using Blender 4.5.

Meshes retain the original material textures and bounds. They are used beyond 600 metres; close views use the original meshes. Decimation is constrained to source geometry to prevent thin disconnected trim from moving below the foundation. The source models and textures keep their existing provenance and licenses; no new external assets are included.

`manifest.json` records source and output face counts. Total: 536,612 to 116,884 faces (78.2% reduction).

Regenerate from the repository root:

```powershell
& 'D:\Program Files\Blender Foundation\Blender 4.5\blender.exe' --background --python scripts/build_city_lod.py
```

The building asset tests check loading, lot bounds, roof height, and ground contact for every selected model.
