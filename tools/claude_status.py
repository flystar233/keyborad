#!/usr/bin/env python3
"""
claude_status.py -- 把 Claude Code 的运行状态通过 USB 串口推给 Keybrick 键盘

用法：作为 Claude Code 的 hook 命令（用 tools/install_claude_status.py 一键安装）。
Claude Code 每次触发事件时运行它，事件信息（JSON）从标准输入传进来，
脚本把它换成一行 "@CLAUDE <状态> [工具名]" 发给键盘。

    SessionStart      -> idle   屏幕显示 "Claude ready"
    UserPromptSubmit  -> work   "Claude thinking"，开始计时
    PreToolUse        -> tool   "Claude <工具名>"（AskUserQuestion / ExitPlanMode 算 wait）
    PostToolUse       -> work
    Notification      -> wait   "Claude NEED YOU" 闪烁 + K5 灯橙闪 + 嘀嘀嘀（空闲提醒忽略）
    Stop              -> done   "Claude done" + 本轮用时 + K5 灯绿闪 + 提示音
    SessionEnd        -> off    隐藏

也可以手动测试：  python claude_status.py work
                  python claude_status.py tool Bash

支持 Windows / macOS / Linux。依赖：pyserial。
可选环境变量：KEYBRICK_PORT=COM3（或 /dev/cu.usbmodem1101）强制指定串口。

设计原则：绝不能拖慢或卡住 Claude Code。
  - 找不到键盘、串口被占用（改键网页 / 串口监视器开着）、任何异常，都直接安静退出，返回 0。
  - 多个 hook 进程按"进程创建时刻"排队发送，晚到的旧消息丢弃（hook 在后台并发运行，
    Python 启动又快慢不一，实际到达顺序不一定等于事件顺序）。
  - 打开串口时 DTR/RTS 保持为低：ESP32-C3 的 USB-Serial/JTAG 会把这两根线当成复位 / 下载模式控制。
  - 每次运行记一行日志到临时目录的 keybrick_claude.log，排查用。
"""
import json
import os
import sys
import tempfile
import time

ESPRESSIF_VID = 0x303A
IS_WINDOWS = os.name == "nt"

_TMP  = tempfile.gettempdir()
CACHE = os.path.join(_TMP, "keybrick_port.txt")        # 上次用过的串口
LOCK  = os.path.join(_TMP, "keybrick_claude.lock")     # 多个 hook 进程排队发送
ORDER = os.path.join(_TMP, "keybrick_claude.order")    # 最近一条已发送消息的进程创建时刻（Unix 秒）
LOG   = os.path.join(_TMP, "keybrick_claude.log")      # 发送记录

EVENT_TO_STATE = {
    "SessionStart": "idle",
    "UserPromptSubmit": "work",
    "PreToolUse": "tool",
    "PostToolUse": "work",
    "Notification": "wait",
    "Stop": "done",
    "SessionEnd": "off",
}

# 这些工具一调用就是在等你回答 / 确认
WAIT_TOOLS = {"AskUserQuestion", "ExitPlanMode"}


# ============================================================================ 事件 -> 命令
def short_tool_name(name):
    # MCP 工具名很长，比如 mcp__claude_ai_Linear__list_issues -> list_issues
    if name.startswith("mcp__"):
        name = name.split("__")[-1]
    name = "".join(c for c in name if 0x20 < ord(c) < 0x7F)   # 只留屏幕能显示的字符，不要空格
    return name[:15] or "tool"


def parse_event(stdin_text):
    try:
        data = json.loads(stdin_text) if stdin_text.strip() else {}
        return data if isinstance(data, dict) else {}
    except ValueError:
        return None


def build_line(argv, data):
    if len(argv) > 1:                       # 手动测试：直接给状态
        state = argv[1]
        arg = argv[2] if len(argv) > 2 else ""
    else:                                   # 作为 hook：从事件 JSON 里取
        event = data.get("hook_event_name", "")
        state = EVENT_TO_STATE.get(event)
        if state is None:
            return None
        tool = data.get("tool_name", "")
        # Claude 主动问你问题 / 等你批准计划：也是"该你了"
        if event == "PreToolUse" and tool in WAIT_TOOLS:
            state = "wait"
        # 空闲提醒（做完后等你输入超过一分钟）不算：done 已经提示过了，不再闪灯、响铃
        if event == "Notification":
            msg = str(data.get("message", "")).lower()
            if data.get("notification_type") == "idle_prompt" or "waiting for your input" in msg:
                return None
        arg = tool if state == "tool" else ""
    arg = short_tool_name(arg) if arg else ""
    return ("@CLAUDE %s %s" % (state, arg)).rstrip() + "\n"


# ============================================================================ 进程创建时刻
def process_start_time():
    """本进程被创建的时刻（Unix 秒）。

    Claude Code 按事件顺序依次启动 hook 进程，但解释器启动要几十到几百毫秒、快慢不一，
    跑到"发送"这一步的先后不等于事件顺序。进程创建时刻是系统在启动瞬间记下的，用它排序。
    取不到时退回到"现在"（macOS 上 Python 启动很快，误差可以接受）。
    """
    try:
        if IS_WINDOWS:
            import ctypes
            from ctypes import wintypes

            class FILETIME(ctypes.Structure):
                _fields_ = [("lo", wintypes.DWORD), ("hi", wintypes.DWORD)]
            c, e, k, u = FILETIME(), FILETIME(), FILETIME(), FILETIME()
            k32 = ctypes.windll.kernel32
            # 64 位 Python 下必须声明类型：GetCurrentProcess() 返回伪句柄 -1，不声明会被截成 32 位
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(FILETIME)] * 4
            k32.GetProcessTimes.restype = wintypes.BOOL
            if k32.GetProcessTimes(k32.GetCurrentProcess(), ctypes.byref(c), ctypes.byref(e),
                                   ctypes.byref(k), ctypes.byref(u)):
                filetime = (c.hi << 32) | c.lo                  # 100ns，从 1601-01-01 起算
                return (filetime - 116444736000000000) / 1e7
        elif sys.platform.startswith("linux"):
            with open("/proc/self/stat") as f:
                start_ticks = int(f.read().rsplit(")", 1)[1].split()[19])
            with open("/proc/stat") as f:
                btime = next(int(l.split()[1]) for l in f if l.startswith("btime"))
            return btime + start_ticks / os.sysconf("SC_CLK_TCK")
    except Exception:
        pass
    return time.time()


# ============================================================================ 跨进程排队
class OrderLock:
    """同一时刻只有一个进程在发；比已经发出去的消息"更旧"的直接丢弃。"""

    def __init__(self, timeout=3.0):
        self.timeout = timeout
        self.fd = None

    def _try_lock(self):
        if IS_WINDOWS:
            import msvcrt
            msvcrt.locking(self.fd, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def __enter__(self):
        self.fd = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o666)
        deadline = time.time() + self.timeout
        while True:
            try:
                self._try_lock()
                return self
            except OSError:
                if time.time() > deadline:
                    os.close(self.fd)
                    raise TimeoutError("lock busy")
                time.sleep(0.02)

    def __exit__(self, *exc):
        try:
            if IS_WINDOWS:
                import msvcrt
                os.lseek(self.fd, 0, 0)
                msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.fd, fcntl.LOCK_UN)
        finally:
            os.close(self.fd)

    @staticmethod
    def last_sent():
        try:
            with open(ORDER, encoding="ascii") as f:
                return float(f.read().strip() or 0)
        except (OSError, ValueError):
            return 0.0

    @staticmethod
    def mark_sent(t):
        try:
            with open(ORDER, "w", encoding="ascii") as f:
                f.write("%.6f" % t)
        except OSError:
            pass


# ============================================================================ 串口
def candidate_ports():
    forced = os.environ.get("KEYBRICK_PORT")
    if forced:
        return [forced]
    ports = []
    try:
        with open(CACHE, encoding="utf-8") as f:
            ports.append(f.read().strip())
    except OSError:
        pass
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID and p.device not in ports:
                ports.append(p.device)
    except Exception:
        pass
    return [p for p in ports if p]


def read_ack(s, timeout=0.5):
    """读键盘对这条命令的回复（@OK / @ERR），中间夹着的普通日志行跳过。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").strip()
        if text.startswith("@OK") or text.startswith("@ERR"):
            return text
    return ""


def send(line, wait_reply=False):
    """发一行并等键盘确认。

    【之前的 bug】hook 模式为了省时间，写完就关串口、不等回复。结果偶尔整条消息丢失，
    或者开头被截掉、只剩尾巴 "...e\\n" 到达键盘 —— 尾巴里的字母会被当成单键诊断命令执行
    （"e" = 切换灯效），表现为：屏幕卡在 thinking、灯效自己乱切。
    现在一律等到 @OK / @ERR 再关串口；没等到就重发一次。
    """
    import serial
    for port in candidate_ports():
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.dtr = False                   # 先设好再 open，打开瞬间也不会拉高
            s.rts = False
            s.timeout = 0.1
            s.write_timeout = 0.5
            s.open()
            try:
                s.reset_input_buffer()      # 丢掉打开前缓冲区里的旧日志
                reply = ""
                for _attempt in range(2):
                    s.write(line.encode("ascii", "replace"))
                    s.flush()
                    reply = read_ack(s)
                    if reply:
                        break
            finally:
                s.close()
            if not reply:
                continue                    # 这个口没人应答：换下一个候选口
            try:
                with open(CACHE, "w", encoding="utf-8") as f:
                    f.write(port)
            except OSError:
                pass
            return port, reply
        except Exception:
            continue
    return None, None


# ============================================================================ 日志
def log(msg):
    try:
        if os.path.exists(LOG) and os.path.getsize(LOG) > 200_000:
            os.replace(LOG, LOG + ".old")
        with open(LOG, "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))
    except OSError:
        pass


# ============================================================================ 发一行（带排队）
def deliver(line, born, wait_reply=False):
    """排队发送一行；born 是这条消息的"发生时刻"，比已发出的更旧就丢弃。"""
    port, reply, note = None, None, ""
    try:
        with OrderLock():
            if born < OrderLock.last_sent():
                note = "stale, dropped"             # 更晚的事件已经发出去了，这条作废
            else:
                port, reply = send(line, wait_reply=wait_reply)
                if port:
                    OrderLock.mark_sent(born)
                else:
                    note = "no keyboard / port busy"
    except Exception as ex:
        note = "error: %s" % ex
    return port, reply, note


# ============================================================================ 打断检测
# 你在 Claude Code 里按 Esc / 点停止 / 拒绝权限并打断时，不会触发 Stop 事件，
# 键盘就会一直停在 "thinking"。但 Claude Code 会立刻往会话记录（transcript，JSONL）
# 里追加一条 "[Request interrupted by user...]"。所以：
#   提交提示词时（UserPromptSubmit）在后台起一个监视进程，盯着这个会话的记录文件；
#   一看到打断记录就发 "idle"（屏幕回到 "Claude ready"，灯和声音都停）。
#   这一轮正常结束（Stop）、下一轮开始、会话关闭时，监视进程自己退出，不会越攒越多。
INTERRUPT_MARK = "[Request interrupted by user"
WATCH_MAX_S    = 4 * 3600                     # 兜底：一个监视进程最多活 4 小时


def turn_file(session_id):
    safe = "".join(c for c in str(session_id) if c.isalnum() or c in "-_")[:64] or "default"
    return os.path.join(_TMP, "keybrick_turn_%s.txt" % safe)


def set_turn(session_id, token):
    try:
        with open(turn_file(session_id), "w", encoding="ascii") as f:
            f.write(token)
    except OSError:
        pass


def get_turn(session_id):
    try:
        with open(turn_file(session_id), encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return ""


def spawn_watcher(transcript, session_id):
    """在后台起监视进程。必须和 hook 进程彻底脱钩：不继承标准输入输出，
    否则 Claude Code 会一直等管道关闭，hook 就"结束不了"。"""
    if not transcript or not os.path.exists(transcript):
        return
    import subprocess
    token = "%d-%d" % (os.getpid(), int(time.time() * 1000))
    set_turn(session_id, token)
    kw = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
              close_fds=True)
    if IS_WINDOWS:
        kw["creationflags"] = 0x08000000 | 0x00000200   # CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
    else:
        kw["start_new_session"] = True
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "--watch",
                      transcript, str(session_id), token], **kw)


def watch(transcript, session_id, token):
    log("pid=%-6d watcher start  %s" % (os.getpid(), os.path.basename(transcript)))
    deadline = time.time() + WATCH_MAX_S
    try:
        f = open(transcript, "rb")
    except OSError:
        return
    with f:
        f.seek(0, os.SEEK_END)                  # 只看提交之后新写入的内容
        buf = b""
        while time.time() < deadline:
            if get_turn(session_id) != token:   # 这一轮正常结束 / 下一轮开始 / 会话关闭
                log("pid=%-6d watcher exit (turn over)" % os.getpid())
                return
            chunk = f.read()
            if not chunk:
                time.sleep(0.4)
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                if INTERRUPT_MARK.encode() not in raw:
                    continue
                try:
                    d = json.loads(raw.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if d.get("type") != "user":
                    continue
                # 用"现在"作为这条消息的时刻：它确实发生在之前所有 hook 事件之后
                port, _, note = deliver("@CLAUDE idle\n", time.time())
                log("pid=%-6d watcher        @CLAUDE idle (interrupted)  port=%s %s" % (
                    os.getpid(), port, note))
                set_turn(session_id, "ended")
                return
    log("pid=%-6d watcher exit (timeout)" % os.getpid())


# ============================================================================ main
def main():
    if len(sys.argv) >= 5 and sys.argv[1] == "--watch":
        watch(sys.argv[2], sys.argv[3], sys.argv[4])
        return

    t0 = time.time()
    born = process_start_time()
    manual = len(sys.argv) > 1

    data = {}
    if not manual:
        try:
            # 按 UTF-8 读字节再解码：Windows 下 sys.stdin 默认按 GBK 解码，里面一有中文
            # （比如 Stop 事件带着 Claude 最后一条回复）就会解成乱码、JSON 解析失败
            raw = sys.stdin.buffer.read().decode("utf-8", "replace")
        except Exception:
            raw = ""
        data = parse_event(raw)
        if data is None:
            log("pid=%-6d bad-json %r" % (os.getpid(), raw[:120]))
            return
    event = "manual" if manual else data.get("hook_event_name", "?")

    # 一轮的开始 / 结束：管理打断监视进程
    if event == "UserPromptSubmit":
        try:
            spawn_watcher(data.get("transcript_path"), data.get("session_id"))
        except Exception as ex:
            log("pid=%-6d spawn watcher failed: %s" % (os.getpid(), ex))
    elif event in ("Stop", "SessionEnd"):
        set_turn(data.get("session_id"), "ended")

    line = build_line(sys.argv, data)
    if not line:
        return

    port, reply, note = deliver(line, born, wait_reply=manual)
    log("pid=%-6d %-17s %-24s port=%s %s (%.0fms after start, %.0fms total)" % (
        os.getpid(), event, line.strip(), port, note, (t0 - born) * 1000, (time.time() - born) * 1000))
    if manual:                              # 手动测试时打印结果，hook 模式下保持安静
        print("sent:", line.strip(), "| port:", port, "| reply:", reply, note)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        log("pid=%-6d CRASH %s" % (os.getpid(), " | ".join(traceback.format_exc().splitlines())))
    sys.exit(0)
