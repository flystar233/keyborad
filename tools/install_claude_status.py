#!/usr/bin/env python3
"""
install_claude_status.py -- 一键安装 / 卸载 "Claude Code 状态 -> Keybrick 键盘"

在任何一台装了 Claude Code 的电脑上（Windows / macOS / Linux），把键盘用 USB 线插上，运行：

    python install_claude_status.py            安装（或更新）
    python install_claude_status.py --uninstall 卸载
    python install_claude_status.py --test      只发一遍测试状态

它会：
  1. 检查 pyserial（必需）和 bleak（可选，没插线时走蓝牙），没有就用当前 Python 自动安装（pip --user）；
  2. 把同目录的 claude_status.py 复制到 ~/.claude/keybrick/（之后可以删掉项目目录）；
  3. 把 7 个 hook 合并进 ~/.claude/settings.json：
       - 只增删本工具自己的条目，其它设置和别的 hook 原样保留；
       - 改之前备份成 settings.json.bak-keybrick；
       - 重复运行不会重复添加（先删旧的再加新的）；
  4. 给键盘发一遍测试状态，确认能收到。

安装后 Claude Code 不用重启，下一次事件就生效；在 Claude Code 里输入 /hooks 可以查看或停用。
本脚本只需要 Python 3.7+ 标准库（pyserial、bleak 会自动装）。
"""
import json
import os
import shutil
import subprocess
import sys
import time

HERE        = os.path.dirname(os.path.abspath(__file__))
SRC_SCRIPT  = os.path.join(HERE, "claude_status.py")
CLAUDE_DIR  = os.path.join(os.path.expanduser("~"), ".claude")
INSTALL_DIR = os.path.join(CLAUDE_DIR, "keybrick")
DST_SCRIPT  = os.path.join(INSTALL_DIR, "claude_status.py")
SETTINGS    = os.path.join(CLAUDE_DIR, "settings.json")
BACKUP      = SETTINGS + ".bak-keybrick"
MARK        = "claude_status.py"          # 用脚本名识别"本工具的 hook 条目"

# 事件 -> 是否放后台运行。
#   工具事件很频繁，放后台不拖慢 Claude Code；
#   开始 / 提交 / 等你 / 完成 / 关闭 同步运行，保证一定送到（每轮只多 0.1~0.3 秒）。
EVENTS = [
    ("SessionStart",     None, False),
    ("UserPromptSubmit", None, False),
    ("PreToolUse",       "*",  True),
    ("PostToolUse",      "*",  True),
    ("Notification",     None, False),
    ("Stop",             None, False),
    ("SessionEnd",       None, False),
]


def say(msg):
    print(msg, flush=True)


def posix(path):
    return path.replace("\\", "/")


# ---------------------------------------------------------------------------- 依赖
def pip_install(pkg):
    r = subprocess.run([sys.executable, "-m", "pip", "install", "--user", "--quiet", pkg])
    if r.returncode != 0:            # 虚拟环境里不允许 --user，去掉再试一次
        subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", pkg])
    import importlib
    importlib.invalidate_caches()


def has_module(name):
    import importlib.util
    return importlib.util.find_spec(name) is not None


def ensure_deps():
    """pyserial 必需（USB）；bleak 可选（没插线时走蓝牙），装不上也不影响 USB。"""
    if has_module("serial"):
        say("[1/4] pyserial 已安装")
    else:
        say("[1/4] 正在安装 pyserial ...")
        pip_install("pyserial")
        if not has_module("serial"):
            say("      !! pyserial 安装失败，请手动执行：%s -m pip install pyserial" % sys.executable)
            return False
        say("      pyserial 安装完成")
    if has_module("bleak"):
        say("      bleak 已安装（没插线时走蓝牙）")
    else:
        say("      正在安装 bleak（可选，用于没插线时走蓝牙）...")
        pip_install("bleak")
        if has_module("bleak"):
            say("      bleak 安装完成")
        else:
            say("      bleak 没装上，只能走 USB；需要蓝牙时手动执行：%s -m pip install bleak" % sys.executable)
    return True


def load_settings():
    if not os.path.exists(SETTINGS):
        return {}
    with open(SETTINGS, encoding="utf-8") as f:
        text = f.read()
    if not text.strip():
        return {}
    try:
        return json.loads(text)
    except ValueError as ex:
        say("!! %s 不是合法的 JSON（%s），为了不弄坏它，安装中止。请先修好这个文件。" % (SETTINGS, ex))
        sys.exit(1)


def save_settings(d):
    os.makedirs(CLAUDE_DIR, exist_ok=True)
    if os.path.exists(SETTINGS):
        shutil.copy2(SETTINGS, BACKUP)
    tmp = SETTINGS + ".tmp-keybrick"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, SETTINGS)


def is_ours(hook):
    text = " ".join([str(hook.get("command", ""))] + [str(a) for a in hook.get("args", [])])
    return MARK in text


def remove_our_hooks(d):
    """删掉本工具的 hook 条目；删空的分组 / 事件也一起清掉。返回删了几条。"""
    removed = 0
    hooks = d.get("hooks")
    if not isinstance(hooks, dict):
        return 0
    for event in list(hooks.keys()):
        groups = hooks[event]
        if not isinstance(groups, list):
            continue
        new_groups = []
        for g in groups:
            inner = g.get("hooks", []) if isinstance(g, dict) else []
            keep = [h for h in inner if not (isinstance(h, dict) and is_ours(h))]
            removed += len(inner) - len(keep)
            if keep:
                g["hooks"] = keep
                new_groups.append(g)
            elif not inner:
                new_groups.append(g)          # 本来就是空的分组：不是我们的，原样保留
        if new_groups:
            hooks[event] = new_groups
        else:
            del hooks[event]
    if not hooks:
        del d["hooks"]
    return removed


def add_our_hooks(d):
    hooks = d.setdefault("hooks", {})
    for event, matcher, background in EVENTS:
        h = {
            "type": "command",
            "command": posix(sys.executable),
            "args": [posix(DST_SCRIPT)],
            "timeout": 15,
        }
        if background:
            h["async"] = True
        group = {"hooks": [h]}
        if matcher:
            group = {"matcher": matcher, "hooks": [h]}
        hooks.setdefault(event, []).append(group)


# ---------------------------------------------------------------------------- 测试
def run_test():
    if not os.path.exists(DST_SCRIPT):
        say("!! 还没安装（找不到 %s）" % DST_SCRIPT)
        return False
    say("给键盘发测试状态（看键盘屏幕最下面一行）：")
    ok = True
    for state, hold in [(["work"], 2), (["tool", "Test"], 2), (["wait"], 3), (["done"], 0)]:
        r = subprocess.run([sys.executable, DST_SCRIPT] + state, capture_output=True, text=True)
        out = (r.stdout or "").strip()
        say("   " + (out or "(无输出)"))
        if "@OK" not in out:
            ok = False
        time.sleep(hold)
    if ok:
        say("测试通过：键盘应该依次显示 thinking -> Test -> NEED YOU（橙闪+嘀嘀嘀）-> done（绿闪）。")
    else:
        say("!! 键盘没有回应 @OK。检查：\n"
            "   - 键盘是否用 USB 数据线连着这台电脑（只连蓝牙收不到状态）\n"
            "   - 串口是否被占用（改键网页、串口监视器、Arduino IDE 等先关掉）\n"
            "   - 键盘固件是否支持 Claude 状态（串口输入 ? 帮助里有 @... 一行）\n"
            "   - Linux 需要串口权限：sudo usermod -aG dialout $USER 后重新登录\n"
            "   - 有多块板子时可以设环境变量 KEYBRICK_PORT 指定串口")
    return ok


# ---------------------------------------------------------------------------- 安装 / 卸载
def install():
    say("安装 Claude Code 状态推送（Python: %s）" % sys.executable)
    if not os.path.exists(SRC_SCRIPT):
        say("!! 找不到 %s：请把 claude_status.py 和本脚本放在同一个目录" % SRC_SCRIPT)
        sys.exit(1)
    if not ensure_deps():
        sys.exit(1)

    os.makedirs(INSTALL_DIR, exist_ok=True)
    if os.path.abspath(SRC_SCRIPT) != os.path.abspath(DST_SCRIPT):
        shutil.copy2(SRC_SCRIPT, DST_SCRIPT)
    say("[2/4] 脚本已复制到 %s" % DST_SCRIPT)

    d = load_settings()
    removed = remove_our_hooks(d)
    add_our_hooks(d)
    save_settings(d)
    say("[3/4] 已写入 %s（%s7 个 hook；原文件备份为 %s）" % (
        SETTINGS, "替换旧的 %d 条，" % removed if removed else "新增 ", os.path.basename(BACKUP)))

    say("[4/4] 测试")
    run_test()
    say("\n完成。Claude Code 不用重启，下一次事件就生效；输入 /hooks 可以查看或停用。")


def uninstall():
    d = load_settings()
    removed = remove_our_hooks(d)
    if removed:
        save_settings(d)
        say("已从 %s 删除 %d 条 hook（原文件备份为 %s）" % (SETTINGS, removed, os.path.basename(BACKUP)))
    else:
        say("settings.json 里没有本工具的 hook")
    if os.path.isdir(INSTALL_DIR):
        shutil.rmtree(INSTALL_DIR, ignore_errors=True)
        say("已删除 %s" % INSTALL_DIR)


if __name__ == "__main__":
    # 输出被重定向（管道 / 日志文件）时统一用 UTF-8，免得中文提示变成乱码
    if not sys.stdout.isatty():
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    if sys.version_info < (3, 7):
        say("需要 Python 3.7 或更新版本")
        sys.exit(1)
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg in ("--uninstall", "-u", "uninstall"):
        uninstall()
    elif arg in ("--test", "-t", "test"):
        run_test()
    else:
        install()
