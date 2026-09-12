from pathlib import Path

from PIL import Image

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
    for name in CAPTURE_FILES:
        path = src / name
        if not path.exists():
            raise FileNotFoundError(path)
        image = Image.open(path).convert("RGB")
        pixels = list(image.getdata())
        total = len(pixels)
        luminance = sum((0.2126 * r + 0.7152 * g + 0.0722 * b) for r, g, b in pixels) / total
        white = sum(1 for r, g, b in pixels if r > 210 and g > 210 and b > 200) * 100.0 / total
        yellow = sum(1 for r, g, b in pixels if r > 180 and g > 150 and b < 120) * 100.0 / total
        roadish = sum(1 for r, g, b in pixels if abs(r - g) < 15 and abs(g - b) < 25 and 70 < r < 190) * 100.0 / total
        print(f"{path.name} lum={luminance:.1f} white={white:.3f}% yellow={yellow:.3f}% roadish={roadish:.1f}%")


if __name__ == "__main__":
    main()