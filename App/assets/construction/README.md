# Construction machinery

Original game models, metres, Y up, +Z forward. Each model uses one material and a 2048 px atlas with baked local ambient occlusion. No manufacturer photographs or logos are embedded.

Models: excavator, road_roller, asphalt_paver, mobile_crane.

Rebuild from the repository root:

```powershell
python scripts/build_construction_models.py
blender -b --python scripts/finish_construction_models_blender.py
```

Python requires NumPy and Pillow; the finish script uses Blender 4.5. OBJ, MTL and PNG are runtime assets. The generated `.blend`, source JSON and verification renders are kept in `artifacts/construction_models/`.

Photographic reference links and observations: `artifacts/road_construction/REFERENCES.md`.
These models are drawn during road construction by RoadRenderer; they are not traffic vehicles or placeable buildings.
