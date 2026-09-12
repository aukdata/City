"""Reproducible, original Japanese town assets. Python 3 + Pillow + NumPy.

OBJ convention: metres, Y up, street/front = -Z, ground origin at Y=0.
One shared diffuse atlas / one material per model; no external downloads.
"""
from pathlib import Path
import json
import math
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "App/assets/buildings/commercial"
REVIEW = ROOT / "artifacts/building_models"
TILE = 128
COLORS = [
    (205, 199, 179), (167, 155, 135), (139, 154, 155), (112, 72, 47),
    (56, 91, 105), (62, 70, 77), (150, 149, 139), (231, 228, 209),
    (35, 117, 111), (169, 55, 44), (217, 173, 71), (42, 48, 49),
    (95, 113, 79), (171, 183, 185), (203, 192, 163), (99, 84, 69),
]
# Original 5x7 block letter artwork, no font files embedded or redistributed.
LETTERS = {
    'A': ['01110','10001','10001','11111','10001','10001','10001'],
    'C': ['01111','10000','10000','10000','10000','10000','01111'],
    'E': ['11111','10000','10000','11110','10000','10000','11111'],
    'F': ['11111','10000','10000','11110','10000','10000','10000'],
    'H': ['10001','10001','10001','11111','10001','10001','10001'],
    'I': ['11111','00100','00100','00100','00100','00100','11111'],
    'K': ['10001','10010','10100','11000','10100','10010','10001'],
    'L': ['10000','10000','10000','10000','10000','10000','11111'],
    'M': ['10001','11011','10101','10101','10001','10001','10001'],
    'N': ['10001','11001','11001','10101','10011','10011','10001'],
    'O': ['01110','10001','10001','10001','10001','10001','01110'],
    'P': ['11110','10001','10001','11110','10000','10000','10000'],
    'R': ['11110','10001','10001','11110','10100','10010','10001'],
    'S': ['01111','10000','10000','01110','00001','00001','11110'],
    'T': ['11111','00100','00100','00100','00100','00100','00100'],
    'U': ['10001','10001','10001','10001','10001','10001','01110'],
    'W': ['10001','10001','10001','10101','10101','11011','10001'],
    'Y': ['10001','10001','01010','00100','00100','00100','00100'],
    ' ': ['00000'] * 7,
}
SIGNS = ['HARU MART', 'KOME', 'CAFE', 'WORKS', 'STORE', 'CLINIC', 'FIRE', 'CITY']


def make_atlas():
    atlas = Image.new('RGB', (TILE * 8, TILE * 4))
    rng = np.random.default_rng(20260911)
    for index in range(32):
        base = COLORS[index % 16]
        noise = rng.normal(0, 2.0, (TILE, TILE, 1))
        pixels = np.clip(np.array(base)[None, None, :] + noise, 0, 255).astype('uint8')
        tile = Image.fromarray(pixels)
        draw = ImageDraw.Draw(tile)
        if index == 1:  # small ceramic wall tiles
            for y in range(0, TILE, 16):
                draw.line((0, y, TILE, y), fill=(137, 129, 116))
                for x in range((y // 16 % 2) * 16, TILE, 32):
                    draw.line((x, y, x, y + 16), fill=(137, 129, 116))
        elif index in (2, 5, 13):
            for x in range(6, TILE, 16):
                draw.line((x, 0, x, TILE), fill=tuple(max(0, c - 22) for c in base), width=2)
                draw.line((x + 2, 0, x + 2, TILE), fill=tuple(min(255, c + 12) for c in base))
        elif index == 3:
            for x in range(8, TILE, 16):
                draw.line((x, 0, x + 3, TILE), fill=(91, 57, 36), width=2)
        elif index == 4:
            draw.polygon([(0, 30), (128, 80), (128, 106), (0, 56)], fill=(75, 110, 123))
            draw.rectangle((0, 108, 128, 128), fill=(39, 65, 75))
        if 16 <= index < 24:
            draw.rectangle((0, 0, 127, 127), fill=COLORS[[8, 3, 9, 5, 8, 8, 9, 5][index - 16]])
            draw.rectangle((5, 25, 122, 102), outline=(229, 224, 205), width=2)
            label = SIGNS[index - 16]
            scale = min(4, 108 // (len(label) * 6))
            start = (128 - (len(label) * 6 - 1) * scale) // 2
            for n, character in enumerate(label):
                for y, row in enumerate(LETTERS[character]):
                    for x, bit in enumerate(row):
                        if bit == '1':
                            px, py = start + (n * 6 + x) * scale, 36 + y * 8
                            draw.rectangle((px, py, px + scale - 1, py + 7), fill=(244, 238, 213))
        atlas.paste(tile, ((index % 8) * TILE, (index // 8) * TILE))
    atlas.save(DEST / 'town_atlas.png')
    return atlas


class Model:
    def __init__(self, stem, title):
        self.stem, self.title = stem, title
        self.faces = []

    def face(self, points, material, uv=None):
        points = np.array(points, dtype=float)
        normal = np.cross(points[1] - points[0], points[2] - points[0])
        length = np.linalg.norm(normal)
        assert length > 1e-8, (self.stem, points)
        normal /= length
        if uv is None:
            uv = [(0, 1), (1, 1), (1, 0), (0, 0)][:len(points)]
        # Four pixel gutters prevent atlas bleed at normal mip levels.
        uv = np.array([((material % 8 * TILE + 4 + u * 120) / 1024,
                        1 - (material // 8 * TILE + 4 + v * 120) / 512) for u, v in uv])
        for i in range(1, len(points) - 1):
            self.faces.append((points[[0, i, i + 1]], uv[[0, i, i + 1]], normal))

    def box(self, x, y, z, width, height, depth, material):
        assert min(width, height, depth) > 0
        x0, x1 = x - width / 2, x + width / 2
        y0, y1 = y, y + height
        z0, z1 = z - depth / 2, z + depth / 2
        for face in [
            [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0)],
            [(x1,y0,z1),(x0,y0,z1),(x0,y1,z1),(x1,y1,z1)],
            [(x0,y0,z1),(x0,y0,z0),(x0,y1,z0),(x0,y1,z1)],
            [(x1,y0,z0),(x1,y0,z1),(x1,y1,z1),(x1,y1,z0)],
            [(x0,y1,z0),(x1,y1,z0),(x1,y1,z1),(x0,y1,z1)],
            [(x0,y0,z1),(x1,y0,z1),(x1,y0,z0),(x0,y0,z0)],
        ]:
            # OBJ uses right-handed counterclockwise outward faces.
            self.face(list(reversed(face)), material)

    def beam(self, start, end, radius, material, sides=8):
        start, end = np.array(start), np.array(end)
        axis = end - start
        axis /= np.linalg.norm(axis)
        right = np.cross(axis, [0, 0, 1] if abs(axis[2]) < .9 else [1, 0, 0])
        right /= np.linalg.norm(right)
        up = np.cross(axis, right)
        ring = [radius * (right * math.cos(i * math.tau / sides) + up * math.sin(i * math.tau / sides)) for i in range(sides)]
        self.face([start + p for p in reversed(ring)], material, [(0,0)] * sides)
        self.face([end + p for p in ring], material, [(0,0)] * sides)
        for i in range(sides):
            j = (i + 1) % sides
            self.face([start + ring[i], start + ring[j], end + ring[j], end + ring[i]], material)

    def window(self, x, y, z, width=1.2, height=1.2):
        self.box(x,y,z,width+.14,height+.14,.15,11)
        self.box(x,y+.07,z-.09,width,height,.025,4)
        for dx in (-width/2, 0, width/2):
            self.box(x+dx,y+.04,z-.12,.045,height+.07,.055,13)
        self.box(x,y-.04,z-.14,width+.23,.08,.24,7)

    def door(self, x, z, width=1.05, material=4):
        self.box(x,.17,z,width+.15,2.25,.14,11)
        self.box(x,.24,z-.09,width,2.1,.03,material)
        self.box(x,.24,z-.12,.045,2.1,.05,13)
        for dx in (-.12,.12):
            self.box(x+dx,1.0,z-.18,.035,.4,.07,7)

    def ac(self, x, y, z):
        self.box(x,y,z,.85,.65,.45,7)
        self.beam((x+.16,y+.34,z-.24),(x+.16,y+.34,z-.26),.23,11,12)
        for i in range(6):
            self.box(x+.16,y+.17+i*.067,z-.28,.42,.025,.025,13)
        self.box(x-.29,y+.16,z-.245,.09,.36,.03,13)
        for dx in (-.3,.3):
            self.box(x+dx,y-.07,z,.08,.07,.58,5)

    def parapet(self, width, depth, y):
        self.box(0,y,0,width-.32,.12,depth-.32,5)
        for x in (-width/2+.08,width/2-.08):
            self.box(x,y,0,.16,.36,depth-.32,7)
        for z in (-depth/2+.08,depth/2-.08):
            self.box(0,y,z,width,.36,.16,7)

    def gable(self, width, depth, eave, rise, material=5):
        x, z = width / 2, depth / 2
        self.face([(-x,eave,-z),(0,eave+rise,-z),(x,eave,-z)],0)
        self.face([(x,eave,z),(0,eave+rise,z),(-x,eave,z)],0)
        self.face([(-x,eave,-z),(-x,eave,z),(0,eave+rise,z),(0,eave+rise,-z)],material)
        self.face([(0,eave+rise,-z),(0,eave+rise,z),(x,eave,z),(x,eave,-z)],material)
        for xx in (-x,x):
            self.beam((xx,eave,-z),(xx,eave,z),.07,13)
        self.beam((0,eave+rise,-z),(0,eave+rise,z),.09,material)
        for zz in (-z,z):
            self.beam((-x,eave,zz),(0,eave+rise,zz),.075,material)
            self.beam((0,eave+rise,zz),(x,eave,zz),.075,material)

    def sign(self, x,y,z,width,height,index):
        self.box(x,y,z,width,height,.15,7)
        self.face([(x-width/2+.03,y+.03,z-.081),(x+width/2-.03,y+.03,z-.081),
                   (x+width/2-.03,y+height-.03,z-.081),(x-width/2+.03,y+height-.03,z-.081)][::-1], index,
                  [(1,.83),(0,.83),(0,.17),(1,.17)][::-1])

    def service_details(self, width, depth, height):
        for x in (-width/2+.15,width/2-.15):
            self.beam((x,.12,depth/2+.09),(x,height-.1,depth/2+.09),.045,13)
        self.box(0,.45,depth/2+.08,.8,1.8,.12,13)
        self.box(.8,1.3,depth/2+.08,.35,.5,.17,5)

    def save(self):
        lines = [f'# Original City asset: {self.title}', '# metres; Y up; front -Z',
                 f'mtllib {self.stem}.mtl', f'o {self.stem}', 'usemtl TownAtlas', 's off']
        for points, uv, normal in self.faces:
            lines += ['v ' + ' '.join(f'{c:.6f}' for c in p) for p in points]
            lines += ['vt ' + ' '.join(f'{c:.6f}' for c in p) for p in uv]
            lines += ['vn ' + ' '.join(f'{c:.6f}' for c in normal)]
        for i in range(len(self.faces)):
            lines.append('f ' + ' '.join(f'{i*3+j+1}/{i*3+j+1}/{i+1}' for j in range(3)))
        (DEST / f'{self.stem}.obj').write_text('\n'.join(lines)+'\n', encoding='utf-8')
        (DEST / f'{self.stem}.mtl').write_text('newmtl TownAtlas\nKa 0.2 0.2 0.2\nKd 1 1 1\nKs 0.08 0.08 0.08\nNs 24\nd 1\nillum 2\nmap_Kd town_atlas.png\n', encoding='utf-8')
        (DEST / f'{self.stem}.toml').write_text('# Metres. Front = -Z. See MODEL_CATALOG.md\nsetback_from_road_m = 1.8\nscale = 1.0\n', encoding='utf-8')
        points = np.concatenate([face[0] for face in self.faces])
        assert points[:,1].min() >= -1e-6
        return {'stem':self.stem,'title':self.title,'triangles':len(self.faces),'materials':1,
                'bounds_min':points.min(axis=0).tolist(),'bounds_max':points.max(axis=0).tolist()}


def shops():
    models = []
    m = Model('shop_001','Neighborhood convenience store')
    m.box(0,0,0,7.8,.18,6.8,6)
    m.box(0,.18,.2,7.4,2.95,5.8,1)
    for x in (-2.8,-1.55,1.55,2.8):
        m.window(x,.5,-2.73,1.12,1.9)
    m.door(0,-2.74,1.45)
    m.box(0,2.6,-2.92,7.65,.42,.48,8)
    m.box(0,2.65,-3.175,7.5,.08,.03,10)
    m.sign(0,2.74,-3.19,3.25,.38,16)
    m.parapet(7.65,6.15,3.13)
    m.ac(-2.3,3.25,1.1); m.ac(-1.25,3.25,1.1)
    # Delivery crates, entrance bollards, drinks machine and recycling bins.
    for x in (-3.0,3.0):
        m.beam((x,.18,-3.2),(x,.9,-3.2),.055,13)
    m.box(3.1,.18,-3.07,.6,1.7,.44,9)
    m.box(3.1,.65,-3.3,.46,.94,.025,4)
    for y in (.86,1.16,1.46):
        for x in (2.96,3.1,3.24):
            m.box(x,y,-3.32,.075,.14,.03,7 if y<1.3 else 10)
    m.box(-3.15,.18,-3.08,.72,.82,.48,13)
    m.box(-3.15,.81,-3.33,.35,.1,.03,11)
    m.service_details(7.4,6.2,3.0)
    models.append(m)

    m = Model('shop_002','Traditional rice shop')
    m.box(0,0,0,6.9,.18,7.5,6)
    m.box(0,.18,.25,6.1,2.65,6.3,0)
    m.gable(6.65,7.1,2.86,.95)
    # Raised seam tiles and ridge caps create a readable roof silhouette.
    for z in np.linspace(-3.5,3.5,19):
        m.beam((-3.31,2.88,z),(0,3.84,z),.035,13,6)
        m.beam((0,3.84,z),(3.31,2.88,z),.035,13,6)
    for x in (-2.86,2.86):
        m.box(x,.18,-2.97,.19,2.68,.22,3)
    m.window(-1.8,.5,-2.94,1.6,1.65)
    m.door(.3,-2.97,1.65,3)
    for x in np.linspace(-2.5,-1.1,8):
        m.box(x,.57,-3.1,.045,1.68,.055,3)
    m.sign(.25,2.3,-3.60,2.7,.45,17)
    m.box(.3,1.96,-3.18,1.7,.43,.04,8)
    for x in (-.25,.3,.85):
        m.box(x,1.96,-3.21,.025,.43,.012,7)
    m.box(2.13,.18,-3.05,1.12,.65,.64,3)
    for x in (1.8,2.12,2.44):
        m.box(x,.83,-3.05,.27,.3,.42,14)
    m.ac(2.4,.3,3.12)
    m.service_details(6.1,6.8,2.7)
    models.append(m)

    m = Model('shop_003','Corner kissaten cafe')
    m.box(0,0,0,7.5,.18,6.8,6)
    m.box(0,.18,.25,6.8,2.8,5.7,1)
    m.parapet(7.05,6.0,2.98)
    for x in (-2.5,-1.12):
        m.window(x,.5,-2.66,1.2,1.72)
    m.door(1.05,-2.67,1.05)
    # Sloped alternating canvas panels with valance.
    for i in range(12):
        x0 = -3.35+i*.56
        mat = 9 if i%2 == 0 else 7
        m.face([(x0,2.25,-3.3),(x0+.56,2.25,-3.3),(x0+.56,2.65,-2.65),(x0,2.65,-2.65)],mat)
        m.box(x0+.28,2.09,-3.29,.56,.16,.05,mat)
    m.sign(0,3.00,-3.10,2.5,.30,18)
    for x in (-2.45,-.85):
        m.box(x,.68,-3.02,.9,.1,.48,3)
        for dx in (-.34,.34):
            m.box(x+dx,.18,-3.02,.06,.5,.35,11)
    m.box(2.65,.18,-3.05,.75,.46,.5,3)
    for x in (2.4,2.65,2.9):
        m.beam((x,.6,-3.05),(x,.95,-3.05),.16,12)
    m.ac(2.3,3.12,1.8)
    m.service_details(6.8,6.2,2.8)
    models.append(m)
    return models


def factories():
    models=[]
    m=Model('factory_001','Small metalworking factory')
    m.box(0,0,0,6.4,.18,6.4,6)
    m.box(0,.18,0,5.9,3.6,5.9,2)
    m.gable(6.3,6.3,3.8,1.35)
    for x in (-2.88,2.88):
        m.box(x,.18,-3.02,.12,3.62,.16,13)
    m.box(-.65,.18,-2.99,3.35,2.8,.13,11)
    m.box(-.65,.23,-3.08,3.17,2.65,.06,13)
    for y in np.arange(.35,2.88,.18):
        m.box(-.65,y,-3.12,3.15,.025,.025,5)
    m.door(2.15,-3.02,.85,13)
    m.sign(-.6,3.15,-3.06,2.55,.48,19)
    for z in (-1.8,0,1.8):
        m.box(3.005,2.1,z,.09,.85,1.25,4)
        m.box(3.055,2.5,z,.06,.05,1.3,13)
    m.beam((1.9,3.7,1.8),(1.9,5.6,1.8),.23,13,12)
    m.beam((1.9,5.58,1.8),(1.9,5.72,1.8),.38,5,12)
    m.service_details(5.9,5.9,3.6)
    models.append(m)
    m=Model('factory_002','Local distribution depot')
    m.box(0,0,0,6.5,.2,6.5,6)
    m.box(0,.2,.25,6.05,3.9,5.6,0)
    m.parapet(6.25,5.85,4.1)
    m.box(0,.2,-2.86,5.6,.42,.65,6)
    for x in (-1.52,1.52):
        m.box(x,.62,-2.59,2.55,2.7,.1,11)
        m.box(x,.65,-2.67,2.26,2.51,.09,2)
        for y in np.arange(.8,3.16,.2):
            m.box(x,y,-2.73,2.26,.03,.04,13)
        for dx in (-1.22,1.22):
            m.box(x+dx,.25,-2.83,.13,.8,.16,10)
    m.box(0,3.35,-2.93,6.3,.17,.65,5)
    m.sign(0,3.63,-2.62,2.3,.42,20)
    for x in (-2.7,2.7):
        m.beam((x,.2,-3.12),(x,1.02,-3.12),.065,10)
    m.ac(1.85,4.25,1.1)
    m.box(-1.2,4.22,1.1,1.25,.7,1.3,13)
    m.service_details(6.05,6.1,4.0)
    models.append(m)
    return models


def public_buildings():
    models=[]
    m=Model('public_001','Neighborhood clinic')
    m.box(0,0,0,6.5,.15,6.5,6)
    m.box(0,.15,.15,6.0,5.9,5.7,7)
    m.box(-2.47,.15,.15,1.1,5.9,5.73,1)
    m.parapet(6.22,5.95,6.05)
    for x in (-1.1,.45,2.0):
        m.window(x,3.8,-2.76,1.1,1.4)
    m.window(-1.7,.9,-2.76,1.7,1.5)
    m.door(.75,-2.77,1.35)
    m.box(.7,2.65,-2.99,2.7,.16,.6,8)
    m.sign(.7,2.96,-2.84,3.6,.6,21)
    m.box(-2.45,4.22,-2.76,.22,.92,.1,8)
    m.box(-2.45,4.56,-2.77,.74,.23,.1,8)
    # Flat entrance paving remains inside the placement footprint.
    m.box(.7,.15,-3.03,2.6,.07,.42,6)
    m.ac(1.8,6.2,1.6); m.ac(.7,6.2,1.6)
    for z in (-1.3,.5,2.2):
        m.box(3.04,3.9,z,.06,1.25,1.1,4)
        m.box(3.08,4.52,z,.04,.05,1.2,13)
    m.service_details(6.0,6.0,5.9)
    models.append(m)
    m=Model('public_002','Volunteer fire station')
    m.box(0,0,0,6.5,.17,6.5,6)
    m.box(-.4,.17,.15,5.0,3.7,5.65,0)
    m.parapet(5.1,5.85,3.87)
    m.box(2.3,.17,1.1,1.25,7.3,3.7,1)
    m.box(2.3,7.47,1.1,1.48,.2,3.95,5)
    m.box(-.65,.18,-2.74,3.55,2.9,.1,11)
    m.box(-.65,.2,-2.83,3.3,2.7,.06,9)
    for y in np.arange(.4,2.85,.22):
        m.box(-.65,y,-2.88,3.28,.026,.026,13)
    m.box(-.65,1.84,-2.9,2.95,.4,.025,4)
    m.sign(-.65,3.23,-2.81,3.0,.48,22)
    m.door(1.8,-2.76,.8,13)
    for y in (4.25,5.65):
        m.window(2.3,y,-.81,.72,.92)
    # Hose-drying tower with roof guard and an antenna.
    for x in (1.65,2.95):
        for z in (-.6,2.8):
            m.beam((x,7.66,z),(x,8.35,z),.035,13)
        m.beam((x,8.35,-.6),(x,8.35,2.8),.035,13)
    m.beam((2.3,7.67,1.8),(2.3,9.0,1.8),.025,13)
    m.beam((-.65,3.87,-2.0),(-.65,4.07,-2.0),.14,9,12)
    m.ac(-1.4,4.04,1.8)
    m.service_details(5.7,6.0,3.6)
    models.append(m)
    return models


def render(model, atlas, back=False, size=760):
    """Orthographic textured OBJ geometry review, not a game screenshot."""
    eye = np.array([10.,8.,-12.] if not back else [-10.,8.,12.])
    forward = -eye / np.linalg.norm(eye)
    right = np.cross(forward,[0.,1.,0.]); right /= np.linalg.norm(right)
    up = np.cross(right,forward)
    basis = np.array([right,up,-forward]).T
    points = np.concatenate([f[0] for f in model.faces])
    center = (points.max(0)+points.min(0))/2
    projected = (points-center) @ basis
    scale = size * .79 / max(np.ptp(projected[:,:2],axis=0))
    buffer = np.full((size,size,3),[233,233,223],dtype=np.uint8)
    depth = np.full((size,size),-np.inf)
    tex = np.array(atlas)
    light=np.array([-3.,8.,-5.]); light/=np.linalg.norm(light)
    def raster(vertices, uv, normal, shadow=False):
        p=(vertices-center)@basis
        p[:,:2] *= [scale,-scale]
        p[:,:2] += [size/2,size*.52]
        lo=np.maximum(np.floor(p[:,:2].min(0)).astype(int),0)
        hi=np.minimum(np.ceil(p[:,:2].max(0)).astype(int),size-1)
        if np.any(lo>hi): return
        xx,yy=np.meshgrid(np.arange(lo[0],hi[0]+1)+.5,np.arange(lo[1],hi[1]+1)+.5)
        a,b,c=p
        denominator=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        if abs(denominator)<1e-7: return
        w0=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/denominator
        w1=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/denominator
        w2=1-w0-w1
        z=w0*a[2]+w1*b[2]+w2*c[2]
        area=np.s_[lo[1]:hi[1]+1,lo[0]:hi[0]+1]
        valid=(w0>=0)&(w1>=0)&(w2>=0)&(z>depth[area])
        if shadow:
            buffer[area][valid]=[206,208,197]
            return
        u=w0*uv[0,0]+w1*uv[1,0]+w2*uv[2,0]
        v=w0*uv[0,1]+w1*uv[1,1]+w2*uv[2,1]
        tx=np.clip((u*tex.shape[1]).astype(int),0,tex.shape[1]-1)
        ty=np.clip(((1-v)*tex.shape[0]).astype(int),0,tex.shape[0]-1)
        shade=.58+.42*max(0,float(normal@light))
        color=np.clip(tex[ty,tx]*shade,0,255).astype('uint8')
        buffer[area][valid]=color[valid]
        depth[area][valid]=z[valid]
    for p,uv,n in model.faces:
        shadow=p.copy()
        shadow[:,0]-=p[:,1]*light[0]/light[1]
        shadow[:,2]-=p[:,1]*light[2]/light[1]
        shadow[:,1]=0
        raster(shadow,uv,n,True)
    for p,uv,n in model.faces:
        if n@(-forward)>0: raster(p,uv,n)
    return Image.fromarray(buffer)


def main():
    DEST.mkdir(parents=True,exist_ok=True); REVIEW.mkdir(parents=True,exist_ok=True)
    atlas=make_atlas()
    models=shops()+factories()+public_buildings()
    manifest=[m.save() for m in models]
    (DEST/'model_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    sheet=Image.new('RGB',(4*520,2*570),(235,235,226))
    draw=ImageDraw.Draw(sheet)
    for i,m in enumerate(models):
        front=render(m,atlas)
        front.save(REVIEW/f'{m.stem}_front.png')
        render(m,atlas,True).save(REVIEW/f'{m.stem}_back.png')
        sheet.paste(front.resize((520,520)),((i%4)*520,(i//4)*570))
        draw.text(((i%4)*520+22,(i//4)*570+521),f'{m.stem} | {m.title}',fill=(40,47,47))
        draw.text(((i%4)*520+22,(i//4)*570+541),f'{len(m.faces)} triangles / 1 material',fill=(82,88,82))
    draw.text((3*520+45,570+180),'CITY / TOWN BUILDINGS\n\n7 original game assets\nMetres / Y up / front -Z\nShared 1024 x 512 atlas\n\nGeometry preview\n(not an in-game capture)',fill=(50,66,60))
    sheet.save(REVIEW/'contact_sheet.png')
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':
    main()
