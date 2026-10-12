"""
ESP32 BLE Keyboard 外壳 v1.3 生成脚本

以 v1.2f 的 shell/cover STL 为底稿做布尔修改（保留外形、底部文字、散热孔），
所有位置都来自 ref/PCB_PCB1_2026-10-11.pdf 的实测坐标。

坐标系：与原 STL 相同。X 以板子中心为 0，Y 向上（USB 一侧为 +Y），Z 为壳体高度。
PCB 坐标 -> 壳体坐标：X = x_pdf - 56.23, Y = 20.605 - y_pdf （已用四个立柱位置校验）。

依赖：pip install numpy trimesh manifold3d shapely networkx rtree
用法：python build_case.py
"""
import os
import numpy as np
import trimesh
from shapely.geometry import Polygon, Point, box as sbox

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "ref")   # 底稿：v1.2f 外壳 STL + PCB 图
SHELL_IN = os.path.join(SRC, "ESP32 BLE Keyboard v1.2f shell.stl")
COVER_IN = os.path.join(SRC, "ESP32 BLE Keyboard v1.2f cover.stl")

# ---------------- 参数 ----------------
PCB_BOTTOM = 9.0          # 立柱顶面 = PCB 底面
PCB_T = 1.6
PCB_TOP = PCB_BOTTOM + PCB_T
COVER_BOTTOM = 16.3       # 盖子底面（与原设计一致）
COVER_TOP = 18.3          # 盖子顶面，与壳沿齐平
POCKET_FLOOR = 17.1       # 盖子顶部按键凹槽底面（原设计 0.8mm 薄层）
INNER = dict(x0=-50.6, x1=50.6, y0=-18.38, y1=15.88)   # 壳体内腔
COVER_GAP = 0.2           # 盖子与壳内壁单边间隙
COVER_CORNER_R = 2.3

# 螺丝：M2 自攻，从壳底穿过立柱 -> PCB 角缺口 -> 拧进盖子凸柱
SCREWS = [(-48.1, 11.43), (48.1, 11.43), (-48.1, -15.875), (48.1, -15.875)]
POST_D = 5.4              # 壳体立柱外径（略吃进内壁，避免与内壁相切产生非流形边）
SCREW_CLEAR_D = 2.4       # 立柱通孔
HEAD_D = 4.4              # 沉头孔直径（M2 圆柱头/盘头 ≤4.0）
HEAD_DEPTH = 2.0          # 沉头孔深度（螺丝头完全藏进底面）
BOSS_D = 4.6              # 盖子凸柱外径
PILOT_D = 1.7             # 自攻底孔
PILOT_TOP = 16.8          # 底孔盲孔顶端（不打穿盖子/凹槽）

# 按键：PCB 实测 19.09mm 间距（原盖子是 19.05 且 Y 偏 0.25）
KEY_X = [18.05, 37.145, 56.24, 75.33, 94.43]
KEY_Y_PDF = 29.595
# 原 14.0 x 15.0。14mm 轴体偏移 0.3mm + 旋转约 3° 时外接尺寸 ≈ 15.3，取 15.4
# 开孔向下一直延伸到 PCB 顶面，作为禁入区，凸柱靠按键一侧会被自动切平
KEY_HOLE = (15.4, 15.6)

# USB-C (USB0) —— PCB 实测中心
USB_X = (12.96 + 22.6) / 2 - 56.23          # = -38.45
USB_Z = PCB_TOP + 3.26 / 2                  # 母座高 3.26
USB_HOLE = (10.0, 3.8, 1.2)                 # 内孔 宽,高,圆角
USB_RECESS = (12.6, 7.0, 3.0, 0.6)          # 外侧插头让位槽 宽,高,圆角,保留内壁厚

# SW1 侧按按键（TS35CA）
SW1_X = (96.18 + 101.18) / 2 - 56.23        # = 42.45
SW1_Z = PCB_TOP + 1.7
SW1_HOLE_D = 3.0          # 圆孔

# OLED 模块占位（避让右上凸柱）
OLED_X1 = 104.04 - 56.23
OLED_Y1 = 20.605 - 10.13


def ext(poly, z0, z1):
    m = trimesh.creation.extrude_polygon(poly, z1 - z0)
    m.apply_translation([0, 0, z0])
    return m


def rrect(cx, cy, w, h, r):
    return sbox(cx - w / 2 + r, cy - h / 2 + r, cx + w / 2 - r, cy + h / 2 - r).buffer(r, 32)


def cyl(x, y, d, z0, z1):
    c = trimesh.creation.cylinder(radius=d / 2, height=z1 - z0, sections=64)
    c.apply_translation([x, y, (z0 + z1) / 2])
    return c


def wall_cut_y(poly, y0, y1):
    """poly 画在 XZ 平面（坐标为 x, z），沿 +Y 拉伸 y0..y1（后墙开孔用）"""
    m = ext(poly, y0, y1)                            # 先沿 Z 拉伸
    # 把 (x, y, z) -> (x, z, y)：多边形平面变成 XZ，拉伸方向变成 Y
    m.vertices = m.vertices[:, [0, 2, 1]]
    m.invert() if m.volume < 0 else None
    m.fix_normals()
    return m


def U(*ms):
    return trimesh.boolean.union(list(ms), engine="manifold")


def D(a, *bs):
    return trimesh.boolean.difference([a, *bs], engine="manifold")


def I(a, b):
    return trimesh.boolean.intersection([a, b], engine="manifold")


def build_shell():
    sh = trimesh.load(SHELL_IN)
    hull = sh.convex_hull

    def fill(x0, x1, z0, z1):
        b = trimesh.creation.box(bounds=[[x0, INNER["y1"], z0], [x1, 30, z1]])
        return I(b, hull)

    # 0) 填平底面原有的刻字（z 0..0.8）
    bottom = trimesh.creation.box(bounds=[[-50, -19, -0.01], [50, 17, 1.0]])
    sh = U(sh, I(bottom, hull))

    # 1) 填掉旧 USB 孔、旧盖子卡扣槽
    sh = U(sh,
           fill(-41.0, -31.0, 10.3, 13.7),          # 旧 USB 孔 x -40.43..-31.53
           fill(-27.0, -19.5, 13.9, COVER_TOP),     # 旧卡扣槽（左）
           fill(19.5, 27.0, 13.9, COVER_TOP))       # 旧卡扣槽（右）

    # 2) 去掉内壁上的卡扣凸点（盖子改用螺丝固定，靠凸柱定高）
    cut = [trimesh.creation.box(bounds=[[INNER["x0"] + 0.01, -15.5, 12.5], [-47.0, 13.0, 19]]),
           trimesh.creation.box(bounds=[[47.0, -15.5, 12.5], [INNER["x1"] - 0.01, 13.0, 19]]),
           trimesh.creation.box(bounds=[[-47.5, INNER["y0"] + 0.01, 12.5], [47.5, -13.5, 19]])]
    sh = D(sh, *cut)

    # 3) 新 USB 孔：内孔 + 外侧插头让位槽
    sh = D(sh,
           wall_cut_y(rrect(USB_X, USB_Z, *USB_HOLE), INNER["y1"] - 1, 30),
           wall_cut_y(rrect(USB_X, USB_Z, *USB_RECESS[:3]), INNER["y1"] + USB_RECESS[3], 30))

    # 4) SW1 按键孔
    sh = D(sh, wall_cut_y(Point(SW1_X, SW1_Z).buffer(SW1_HOLE_D / 2, 32), INNER["y1"] - 1, 30))

    # 5) 立柱：实心化 + 通孔 + 底部沉头孔
    posts = [cyl(x, y, POST_D, 2.0, PCB_BOTTOM) for x, y in SCREWS]
    sh = U(sh, *posts)
    holes = []
    for x, y in SCREWS:
        holes.append(cyl(x, y, SCREW_CLEAR_D, -1, PCB_BOTTOM + 1))
        holes.append(cyl(x, y, HEAD_D, -1, HEAD_DEPTH))
    sh = D(sh, *holes)
    return sh


def build_cover():
    old = trimesh.load(COVER_IN)
    dy = -0.505   # 原盖子文件 Y 方向与壳体差 0.505

    outline = rrect(0, (INNER["y0"] + INNER["y1"]) / 2,
                    INNER["x1"] - INNER["x0"] - 2 * COVER_GAP,
                    INNER["y1"] - INNER["y0"] - 2 * COVER_GAP, COVER_CORNER_R)
    cv = ext(outline, COVER_BOTTOM, COVER_TOP)

    # 顶部凹槽：沿用原盖子形状
    sec = old.section(plane_origin=[0, 0, 3.4], plane_normal=[0, 0, 1])
    loops = sorted(sec.discrete, key=lambda e: Polygon(e[:, :2]).area)
    pocket = Polygon(loops[0][:, :2] + [0, dy])
    # 原 OLED 窗口、小孔
    sec2 = old.section(plane_origin=[0, 0, 2.4], plane_normal=[0, 0, 1])
    oled = small = None
    for e in sec2.discrete:
        p = Polygon(e[:, :2] + [0, dy])
        w = p.bounds[2] - p.bounds[0]
        if 20 < w < 30:
            oled = p
        elif w < 2:
            small = p
    cuts = [ext(pocket, POCKET_FLOOR, COVER_TOP + 1),
            ext(oled, COVER_BOTTOM - 1, COVER_TOP + 1),
            ext(small, COVER_BOTTOM - 1, COVER_TOP + 1)]
    cv = D(cv, *cuts)

    # 螺丝凸柱：从盖子底面一直压到 PCB 顶面
    bosses = [cyl(x, y, BOSS_D, PCB_TOP, COVER_BOTTOM + 0.01) for x, y in SCREWS]
    cv = U(cv, *bosses)
    # 按键开孔一直切到 PCB 顶面（按键禁入区），顺带把凸柱靠按键一侧切平
    ky = 20.605 - KEY_Y_PDF
    keys = [ext(rrect(kx - 56.23, ky, *KEY_HOLE, 0.5), PCB_TOP - 1, COVER_TOP + 1) for kx in KEY_X]
    # 右上凸柱避让 OLED 模块角
    oled_keep = trimesh.creation.box(bounds=[[40, 0, PCB_TOP - 1], [OLED_X1 + 0.3, OLED_Y1 + 0.3, COVER_BOTTOM - 0.01]])
    pilots = [cyl(x, y, PILOT_D, PCB_TOP - 1, PILOT_TOP) for x, y in SCREWS]
    cv = D(cv, *keys, oled_keep, *pilots)
    return cv


if __name__ == "__main__":
    shell = build_shell()
    cover = build_cover()
    out = os.path.join(HERE, "out")
    os.makedirs(out, exist_ok=True)
    shell.export(os.path.join(out, "ESP32 BLE Keyboard v1.3 shell.stl"))
    # 打印方向：顶面朝下，凸柱朝上
    pc = cover.copy()
    pc.apply_transform(trimesh.transformations.rotation_matrix(np.pi, [1, 0, 0]))
    pc.apply_translation([0, 0, -pc.bounds[0][2]])
    pc.export(os.path.join(out, "ESP32 BLE Keyboard v1.3 cover.stl"))
    for f in os.listdir(out):
        m = trimesh.load(os.path.join(out, f))   # 重新读回校验（STL 精度下仍需是封闭实体）
        print(f, "is_volume", m.is_volume, "bounds", m.bounds.round(2).tolist(), "vol", round(m.volume, 1))
