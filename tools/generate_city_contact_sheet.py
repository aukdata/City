from pathlib import Path

from PIL import Image, ImageDraw

CAPTURE_FILES = [
    "city_render_01_overall.png",
    "city_render_02_city.png",
    "city_render_03_close.png",
    "city_render_04_rural_fringe.png",
    "city_render_05_intersection_overview.png",
    "city_render_06_intersection_zoom_a.png",
    "city_render_07_intersection_zoom_b.png",
    "city_render_08_intersection_zoom_c.png",
    "city_render_09_intersection_zoom_d.png",
    "city_render_10_intersection_topdown_a.png",
    "city_render_11_intersection_topdown_b.png",
    "city_render_12_intersection_zoom_e.png",
    "city_render_13_intersection_zoom_f.png",
    "city_render_14_intersection_marking_low_a.png",
    "city_render_15_intersection_marking_low_b.png",
    "city_render_16_intersection_marking_low_c.png",
    "city_render_17_intersection_topdown_c.png",
    "city_render_18_intersection_topdown_d.png",
    "city_render_19_intersection_topdown_e.png",
    "city_render_20_street_corner.png",
    "city_render_21_coastal_buffer.png",
    "city_render_22_rural_landuse.png",
]


def main() -> None:
    src = Path(r"D:\Users\Takuma\Creations\codes\City\App\Screenshot\city_generation")
    files = [src / name for name in CAPTURE_FILES]
    missing = [path.name for path in files if not path.exists()]
    if missing:
        raise FileNotFoundError(f"Missing capture files: {', '.join(missing)}")

    thumb_w, thumb_h = 360, 203
    label_h = 28
    thumbs = []

    for path in files:
        image = Image.open(path).convert("RGB")
        image.thumbnail((thumb_w, thumb_h))
        canvas = Image.new("RGB", (thumb_w, thumb_h + label_h), (28, 28, 28))
        canvas.paste(image, ((thumb_w - image.width) // 2, (thumb_h - image.height) // 2))
        draw = ImageDraw.Draw(canvas)
        draw.text((8, thumb_h + 7), path.name, fill=(235, 235, 235))
        thumbs.append(canvas)

    cols = 2
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * thumb_w, rows * (thumb_h + label_h)), (18, 18, 18))
    for index, thumb in enumerate(thumbs):
        sheet.paste(thumb, ((index % cols) * thumb_w, (index // cols) * (thumb_h + label_h)))

    out = src / "contact_sheet.png"
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()