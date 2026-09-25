import json
import math
import re
from pathlib import Path
from PIL import Image, ImageStat

ROOT = Path.cwd()
FOLDER = ROOT / "App/Screenshot/realism_review_20260926_round20"
LOG = (ROOT / "App/debug.log").read_text(encoding="utf-8-sig", errors="ignore")
OUT = ROOT / "artifacts/realism_review_20260926"
VIEWS = [
    (1, "都市全景", "郊外まで格子状の街区が均一で、土地利用の境界が急"),
    (3, "市街地近景", "街区の内部と住宅の間に広い草地が残る"),
    (4, "田園境界", "住宅地と農地の境界が直線的で、畑の用途変化が単調"),
    (7, "交差点B", "道路の先の区画に未利用の草地が目立つ"),
    (8, "交差点C", "住宅の配置が同じ間隔で続く"),
    (9, "交差点D", "建物の間の緑地が所有区画として読み取りにくい"),
    (12, "交差点E", "商業区画と住宅区画の変化が急"),
    (13, "交差点F", "空き区画がまとまって見える"),
    (21, "海岸", "海岸の近くの畑と林が単調に切り替わる"),
    (22, "田園の土地利用", "畑の矩形と色の並びが反復的"),
    (25, "里山", "斜面の植生密度と地形変化が弱い"),
    (26, "地方の町", "幹線沿い一列に店舗が並び、背後の市街地が疎"),
    (27, "旧市街から郊外", "中心部の格子と郊外の境界が明瞭すぎる"),
    (28, "郊外の街路", "道路に囲まれた広い空白地が残る"),
    (29, "農道", "道の行き先となる農地や施設が少ない"),
    (31, "都市型コンビニ", "駐車区画線や歩道から店舗への導線が弱い"),
    (32, "郊外型コンビニ", "建物の周囲が広い空き地で、利用中の駐車場に見えにくい"),
    (33, "都市型給油所", "給油所周辺の敷地と建物の密度が低い"),
    (34, "郊外型給油所", "道路沿いの施設が単独で孤立している"),
    (35, "農家", "一軒家が周囲の農地や集落と結び付いて見えにくい"),
    (36, "山の中の川", "川面が均一で、瀬や岩など流れの変化が乏しい"),
    (37, "山の集落", "家は写るが、山麓の集落としては周囲の斜面と家並みが弱い"),
    (38, "堤防道路", "河原と堤防はあるが、堤防道の連続が画面内で分かりにくい"),
]
locations = {}
visible = {}
current = None
for line in LOG.splitlines():
    m = re.search(r"\[CaptureLocation\] view=(\d+) focus=\(([^)]+)\)", line)
    if m:
        current = int(m.group(1)) + 1
        locations[current] = [float(v) for v in m.group(2).split(", ")]
    m = re.search(r"\[BuildingVisibility\] submitted=(\d+) considered=(\d+)", line)
    if m and current is not None:
        visible[current] = [int(m.group(1)), int(m.group(2))]
files = sorted(FOLDER.glob("city_render_*.png"))
assert len(files) == 38 and len(locations) == 38, (len(files), len(locations))
photos = []
for number, label, issue in VIEWS:
    path = next(FOLDER.glob(f"city_render_{number:02d}_*.png"))
    with Image.open(path) as image:
        size = image.size
        small = image.convert("RGB").resize((96, 54))
        stats = ImageStat.Stat(small)
    assert size == (1920, 1080), (path, size)
    assert stats.stddev[0] > 4, (path, stats.stddev)
    photos.append({
        "view": number, "label": label, "file": path.relative_to(ROOT).as_posix(),
        "focusXZ": [round(locations[number][0], 1), round(locations[number][2], 1)],
        "buildingsSubmitted": visible.get(number, [0, 0])[0],
        "visualIssue": issue,
    })
pairs = sorted((math.dist(a["focusXZ"], b["focusXZ"]), a["view"], b["view"])
               for i, a in enumerate(photos) for b in photos[i + 1:])
validation = (FOLDER / "validation_report.txt").read_text(encoding="utf-8-sig")
assert "passed=true" in validation
summary = {
    "reviewedPhotos": len(photos), "capturedPhotos": len(files),
    "nearestPairMeters": round(pairs[0][0], 1), "nearestPair": [pairs[0][1], pairs[0][2]],
    "constraintValidation": True,
    "visualIssuesRemaining": len(photos),
}
OUT.mkdir(exist_ok=True)
(OUT / "audit.json").write_text(json.dumps({"summary": summary, "photos": photos}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
lines = [
    "# 38地点の再撮影と23地点の景観レビュー",
    "",
    "Seed 42、1920×1080。23地点は互いに別の場所で、最短の撮影中心間隔は {} m。生成制約検査は通過。".format(summary["nearestPairMeters"]),
    "",
    "## 各写真の指摘",
    "",
    "| 写真 | 場所 | 残る不自然さ |",
    "|---|---|---|",
]
for photo in photos:
    relative = photo["file"]
    lines.append(f"| [{photo['view']:02d}](../../{relative}) | {photo['label']} | {photo['visualIssue']} |")
lines += [
    "",
    "## この反復で修正した点",
    "",
    "- 川の深い切れ込みをなだらかな肩にし、平地の太い川に河原と堤防を追加した。",
    "- 堤防道路を既存道路へ両端接続した4路線として生成した。",
    "- 都心の区画を詰め、農村の家並みと小規模店舗・公園・駐車場の混在を増やした。",
    "- 建物の正面は道路中心線ではなく道路の端までの距離で選び、広い道路を優先した。",
    "- 土地の頂点を追加・移動・削除できる操作を追加し、3点以上と交差防止を検証した。",
    "",
    "## 判定",
    "",
    "数値上の生成制約と撮影枚数は通過した。上表の景観上の指摘は残っているため、無指摘には達していない。",
    "",
]
(OUT / "report.md").write_text("\n".join(lines), encoding="utf-8")
print(json.dumps(summary, ensure_ascii=False))
