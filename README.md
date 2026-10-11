# BLE Keybrick —— ESP32-C3 智能桌面小键盘

一块 5 键蓝牙快捷键盘的固件。连上电脑或手机后，5 个键发送可切换的快捷键组合，
另外带 OLED 状态屏、5 颗 RGB 灯和锂电池供电。

- **蓝牙 HID 键盘**：免驱，支持普通快捷键和媒体键（音量、播放/暂停等）
- **5 组键位预设**：随时切换，掉电保存；用本地网页就能改键，不用重新烧录
- **OLED 状态屏**（128×32）：电池电量 + 电压、蓝牙状态、按键提示
- **5 颗 WS2812B 灯**：6 种灯效，按键反馈，连接 / 低电状态提示
- **蜂鸣器**：开机 / 连接 / 断开 / 确认提示旋律，按键音，内置彩蛋音乐，三档音量开关
- **锂电池**：电量按放电曲线换算，低电提醒，TP4056 充电
- **Claude Code 状态灯**：电脑上的 Claude Code 在思考、跑工具、等你操作、做完了，键盘屏幕 / 灯 / 蜂鸣器实时提示

硬件依据：原理图 `Schematic1` / `Board1` V1.0（嘉立创 EDA）。许可证：[GPL-3.0](LICENSE)。

---

## 目录

1. [快速上手](#1-快速上手)
2. [按键操作](#2-按键操作)
3. [屏幕](#3-屏幕)
4. [灯效](#4-灯效)
5. [电池](#5-电池)
6. [Claude Code 状态](#6-claude-code-状态)
7. [编译与烧录](#7-编译与烧录)
8. [自定义](#8-自定义)
9. [串口诊断命令](#9-串口诊断命令)
10. [故障排查](#10-故障排查)
11. [硬件与引脚](#11-硬件与引脚)
12. [代码结构](#12-代码结构)
13. [开发须知：这块板的几个坑](#13-开发须知这块板的几个坑)
14. [致谢](#14-致谢)
15. [许可证](#15-许可证)

---

## 1. 快速上手

1. **上电**：5 颗灯依次亮 红 → 绿 → 蓝 → 白（自检，约 1 秒），OLED 亮起。
2. **等待配对**：5 颗灯**黄色明暗呼吸** = 正在广播。
3. **配对**：在电脑 / 手机的蓝牙设置里找到 **`ESP32-C3 Keybrick`** 并连接。
4. **连上后**：蜂鸣器"嘀"一声，5 颗灯**绿色闪 2 下后熄灭**，屏幕右上角蓝牙图标旁出现一个实心点。
5. **开始用**：按 K1~K5 发送当前预设的快捷键；按哪个键，哪颗灯亮一下再渐暗，
   屏幕同时显示 1.5 秒按键说明（如 `K1: Ctrl+C`）。

断开后会自动重新广播（约 3~4.5 秒后），灯回到黄色呼吸，主机可以直接回连。

---

## 2. 按键操作

板上一共 6 个按键：

| 按键 | 作用 |
| --- | --- |
| **K1~K5** | 发送当前预设的快捷键（按下立即发送，按住不连发） |
| **U4 短按** | 切换灯效（见 [4. 灯效](#4-灯效)） |
| **U4 长按 1 秒** | 进入 / 退出 **预设切换** 模式（响一声） |

### 2.1 内置的 5 组预设

| # | 预设 | K1 | K2 | K3 | K4 | K5 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | **Daily** | Ctrl+C 复制 | Ctrl+V 粘贴 | Ctrl+Z 撤销 | PrtSc 截图 | Backspace 退格 |
| 2 | **Win Combos** | Alt+Tab | Win+D 显示桌面 | Ctrl+Alt+Del | — | — |
| 3 | **VSCode** | Ctrl+K+C 注释 | Ctrl+K+U 取消注释 | Ctrl+[ 左缩进 | Ctrl+] 右缩进 | — |
| 4 | **LCEDA**（立创 EDA） | Alt+W 布线 | Alt+V 过孔 | Alt+E 铺铜 | Alt+T 顶层 | Alt+B 底层 |
| 5 | **Media** | 音量 − | 音量 + | 播放 / 暂停 | 静音 | 下一曲 |

`—` 表示空键：按下有灯光和声音反馈，但不发送任何东西。
想改键位见 [8.1 用网页改键](#81-用网页改键)。

### 2.2 切换预设

1. **长按 U4 1 秒**，听到"嘀"一声进入预设切换模式：5 颗灯整体偏青色，
   屏幕显示 `> Config Preset` 和当前预设名，下方每 2 秒滚动显示各键说明。
2. **K1 / K2** 上一组 / 下一组（按住可以连续翻页）。翻到的预设**立即生效**，可以直接试按。
3. **K3 确认**：保存到 flash（断电不丢），"叮咚"一声回到正常模式。

预设切换模式下另外两个键：

| 键 | 作用 |
| --- | --- |
| **K4** | 声音档位：全开 → 关按键音 → 静音 → 全开（立即保存，见 [2.3](#23-声音)） |
| **K5** | 播放彩蛋音乐，《欢乐颂》（约 21 秒）《卡农》快速段（约 22 秒）轮流；放的时候再按一下停止 |

不确认就退出（再次长按 U4，或串口 `n`），会**恢复成进来之前的预设**。
预设切换模式下按键**不会**向电脑发送快捷键。

### 2.3 声音

| 场合 | 声音 |
| --- | --- |
| 开机 | do-mi-sol-do 上行 |
| 蓝牙连上 / 断开 | 上扬两音 / 下沉三音 |
| 进入预设切换 / 不确认退出 | 短促上行 / 短促下行 |
| 确认预设 | "叮咚" |
| 按 K1~K5 发送快捷键 | 很短的"嗒" |
| 预设切换模式按 K5 | 彩蛋音乐（见下表） |

彩蛋音乐（预设切换模式下按 K5 轮流播放，播放中再按一次停止）：

| 曲子 | 时长 | 内容 | 音域 |
| --- | --- | --- | --- |
| 《欢乐颂》 | 约 21 秒 | 完整主题四句 A - A' - B - A' | 784~1568Hz |
| 《卡农》 | 约 22 秒 | 第一小提琴声部的十六分音符快速段，放慢后完整重复两遍，收在主音 D | 587~1760Hz |

声音档位（预设切换模式按 K4 切换，屏幕第 1 行显示 `Sound:ON / QUIET / OFF`）：

| 档位 | 按键音 | 提示旋律 | 彩蛋音乐 |
| --- | --- | --- | --- |
| `ON`（默认） | ✓ | ✓ | ✓ |
| `QUIET` | — | ✓ | ✓ |
| `OFF` | — | — | — |

蜂鸣器是无源电磁式（QMB-09B-03，谐振约 2.7kHz），只能放单音旋律、音色是电子方波音。
它在 1~4kHz 最响、低于 1kHz 会稍微发闷，太高又刺耳，所以曲子都放在 600~1800Hz 左右。

改曲子都在 `src/buzzer.cpp`：

- **快慢**：`ODE_Q`（欢乐颂，330）和 `CANON_Q`（卡农，600）是四分音符的毫秒数，越大越慢、曲子也越长。
- **卡农只放一遍**：删掉 `kCanon` 里第二个 `CANON_PASSAGE,`（约 13 秒）。
- **加新曲子**：照 `kOdeToJoy` 的格式写一个 `{频率, 毫秒}` 数组，再加进 `kSongs[]`，K5 就会轮到它。

---

## 3. 屏幕

### 正常模式

平时只有顶部一行状态，其余留空：

```
[电池] 3.87V                              [蓝牙]●

 K1: Ctrl+C          ← 按键时显示 1.5 秒
Claude Bash      1:23 ← Claude Code 状态（见 6. Claude Code 状态）
```

| 元素 | 说明 |
| --- | --- |
| 电池图标（左上） | 5 档填充表示电量；**空心** = 未启用电池检测（见 [5. 电池](#5-电池)） |
| 电压（图标右侧） | 如 `3.87V`，每 10 秒更新；读不到时显示 `-.--V`；未启用电池检测时不显示 |
| 蓝牙图标（右上） | 只有图标 = 等待配对；图标 + 右侧实心点 = 已连接 |
| 按键提示（第 3 行） | 按 K1~K5 时显示键名和功能，1.5 秒后消失 |
| Claude 状态（第 4 行） | 电脑上 Claude Code 的运行状态 + 本轮用时；没在用时不显示 |

### 预设切换模式

第 1 行 `> Preset  Sound:ON`（带当前声音档位），第 2 行 `< 预设名 >`，第 3、4 行轮流显示 5 个键的说明（每 2 秒滚动）。

### 息屏

- 无操作 20 秒变暗，30 秒关屏；按任意键（包括 U4）唤醒。
- 低电时强制变暗省电。

---

## 4. 灯效

### 状态提示（优先于灯效模式）

| 状态 | 灯 |
| --- | --- |
| 上电自检 | 红 → 绿 → 蓝 → 白，各 250ms |
| 未连接蓝牙（上电、广播中、断线后） | 5 颗一起**黄色明暗呼吸** |
| 刚连上蓝牙 | 5 颗一起**绿色闪 2 下**，然后进入当前灯效 |
| 预设切换模式 | 整条偏**青色** |
| 电池低电 | 整条**暗红闪烁**（最高优先级） |

### 灯效模式（6 种）

**短按 U4** 切换到下一种（松开时生效）。上电默认第 1 种，不保存。

| # | 名称 | 效果 |
| --- | --- | --- |
| 1 | `KEYS`（默认） | 全灭；按下的键对应的灯**亮一下再渐暗** |
| 2 | `WHITE` | 白色常亮 |
| 3 | `BREATHE` | 青蓝色整条呼吸 |
| 4 | `RAINBOW_CHASE` | 炫彩跑马灯：一颗亮点来回跑、边跑边换色，带拖尾 |
| 5 | `RAINBOW` | 彩虹流动：整条色轮缓慢平移 |
| 6 | `KEYS_COLOR` | 缤纷按键：全灭；按键时亮一个随机颜色再渐暗 |

- **按键反馈**在所有灯效下都有：平时是白色，`WHITE` 灯效下为青色，`KEYS_COLOR` 下为随机色。
- 未连接蓝牙时切换灯效，会先**预览新灯效 3 秒**再回到黄色呼吸。
- 切换灯效不会改变屏幕内容。
- **省电**：5 颗灯全灭超过 1 分钟，自动切断灯珠电源（GPIO21）；之后一有要亮的灯（按键、连接状态、切灯效等）自动重新上电，只多 5ms 延迟。

---

## 5. 电池

### 显示

- **电量**按锂电池放电曲线查表换算（不是线性），参考点：

  | 电压 | 4.20V | 4.00V | 3.87V | 3.80V | 3.75V | 3.70V | 3.60V | 3.40V |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | 电量 | 100% | 80% | 60% | ≈43% | ≈27% | ≈14% | 5% | 0% |

  锂电池的"3.7V 标称"是整个放电过程的平均电压，不代表还剩一半电。
  空电点取 3.40V：板上 LDO 输出 3.3V，电池再低，系统供电就不稳了。

- **低电**：低于 3.50V 进入（灯暗红闪、屏幕变暗），回升到 3.60V 以上才解除。
- 电量同时通过蓝牙电池服务上报给主机。

### 充电时读数偏高

电压是在 BAT+ 上测的。充电时充电电流会在电池内阻上产生额外压降，
所以显示的电压和电量**偏高**（比如静置 3.70V 的电池充电时可能显示 3.80V 左右，接近充满时停在约 4.2V）。
拔掉充电线静置一会儿后的读数才准确。

### 启用 / 停用电池检测

TP4056 在**没装电池**时也会让 BAT+ 一直保持 4.2V，软件无法区分"没装电池"和"已充满"。
所以用开关明确告诉固件有没有装电池：

| 方式 | 用法 |
| --- | --- |
| `include/board_config.h` | `#define BATTERY_PRESENT 1`（装了电池，默认）/ `0`（没装） |
| 运行时 | 串口发 `v` 来回切换（重启后恢复为上面的设定） |

停用时不采样，电池图标为空心、不显示电压，也不会误报低电。

### 校准

如果显示的电压和万用表实测不一致，调 `board_config.h` 里的 `BATTERY_CAL`：
`显示电压 = ADC 读数 / 0.5 × BATTERY_CAL`。

---

## 6. Claude Code 状态

在电脑上用 [Claude Code](https://claude.com/claude-code) 时，键盘会实时显示它在干什么，
让你不用一直盯着终端：

| Claude Code 在 | 屏幕第 4 行 | 灯 | 声音 |
| --- | --- | --- | --- |
| 刚打开 | `Claude ready` | — | — |
| 思考 | `Claude thinking  0:12`（本轮已用时） | — | — |
| 调用工具 | `Claude Bash      0:15`（工具名） | — | — |
| **等你操作**（批准权限、回答问题、确认计划） | `Claude NEED YOU` 闪烁 | **K5 那颗灯橙色闪** | 嘀嘀嘀 |
| **做完了** | `Claude done      2:47`（本轮总用时） | K5 那颗灯绿闪 3 下 | 三音上行 |
| 你中途打断（Esc / 停止 / 拒绝权限并打断） | 回到 `Claude ready` | 灯停 | — |
| 关闭 | 不显示 | — | — |

- "做完了"会保留 10 分钟，之后变回 `Claude ready`。
- 声音受声音档位控制（[2.3](#23-声音)），`OFF` 档不响。
- 只在正常模式的屏幕上显示；状态变成"等你 / 做完"时会自动点亮屏幕。

### 工作原理

Claude Code 的 **hooks**（事件钩子）在每次提交提示词、调用工具、请求权限、完成时，
运行 `tools/claude_status.py`。脚本把事件换成一行 `@CLAUDE <状态> [工具名]`，
通过 USB 串口发给键盘。工具调用的钩子在后台运行，**不会拖慢 Claude Code**；
键盘没插、串口被占用时脚本直接安静退出。

### 安装（任何一台电脑，一条命令）

支持 Windows / macOS / Linux。需要：装了 Claude Code、有 Python 3.7+、键盘用 **USB 数据线**连着这台电脑
（状态走 USB 串口，只连蓝牙收不到）。

1. 把 `tools/claude_status.py` 和 `tools/install_claude_status.py` 这两个文件拷到那台电脑（放同一个目录）。
2. 运行：

   ```bash
   python install_claude_status.py          # macOS / Linux 上可能是 python3
   ```

安装脚本会：

1. 检查 pyserial，没有就自动安装；
2. 把 `claude_status.py` 复制到 `~/.claude/keybrick/`（装完后那两个文件可以删掉）；
3. 把 7 个钩子合并进 `~/.claude/settings.json`：只增删本工具自己的条目，其它设置和别的钩子原样保留，
   改之前备份成 `settings.json.bak-keybrick`，重复运行不会重复添加；
4. 给键盘发一遍测试状态（thinking → Test → NEED YOU → done），确认能收到。

装完 Claude Code 不用重启，下一次事件就生效。

| 命令 | 作用 |
| --- | --- |
| `python install_claude_status.py` | 安装或更新（改了 `claude_status.py` 后重新运行一次即可） |
| `python install_claude_status.py --test` | 只发一遍测试状态 |
| `python install_claude_status.py --uninstall` | 卸载：删掉钩子和 `~/.claude/keybrick/` |

钩子在 `settings.json` 里长这样（安装脚本自动写，一般不用手动改）：

```json
"Stop": [{ "hooks": [{ "type": "command", "command": "<python 路径>",
                       "args": ["~/.claude/keybrick/claude_status.py"], "timeout": 15 }] }]
```

工具事件（`PreToolUse` / `PostToolUse`）额外带 `"async": true`、`"matcher": "*"`。

- **手动测试**：`python ~/.claude/keybrick/claude_status.py work`（也可以是 `tool Bash` / `wait` / `done`），会打印键盘的回复。
- **指定串口**：默认自动找乐鑫（VID 0x303A）的串口；有多块板子时设环境变量 `KEYBRICK_PORT`
  （Windows 如 `COM3`，macOS 如 `/dev/cu.usbmodem1101`）。
- **Linux 串口权限**：`sudo usermod -aG dialout $USER`，重新登录后生效。
- **暂时关掉**：Claude Code 里输入 `/hooks` 可以查看、停用这些钩子。
- **注意**：改键网页或串口监视器开着时串口被占用，这期间的状态发不过去（不影响 Claude Code）。
- **打断检测**：Claude Code 被打断时不会触发"完成"事件，所以每次提交提示词时会在后台起一个小进程，盯着这个会话的记录文件，看到打断记录就把键盘切回 `Claude ready`；这一轮正常结束或下一轮开始时它自动退出。
- **排查**：每次发送都记在临时目录的 `keybrick_claude.log`（Windows 是 `%TEMP%\keybrick_claude.log`），
  记录事件、状态、串口、是否被丢弃、耗时。
- **顺序保证**：多个钩子进程按"进程创建时刻"排队发送，晚到的旧消息直接丢弃；
  "开始 / 提交 / 等你 / 完成 / 关闭"这几个钩子同步运行，保证一定送到，工具事件在后台运行不拖慢 Claude Code。

---

## 7. 编译与烧录

### 环境

- [PlatformIO](https://platformio.org/)（VS Code 插件或命令行）
- 工具链：`espressif32@6.10.0`（arduino-esp32 2.0.17），首次编译会自动下载
- 板子用 USB-C 直接连电脑（芯片内置 USB-Serial/JTAG，无需 USB 转串口芯片）

### 命令（在项目根目录执行）

```powershell
# 只编译
pio run

# 编译 + 烧录（自动找串口）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1

# 指定串口 / 跳过编译
powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM16
powershell -NoProfile -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM16 -SkipBuild

# 看日志（115200）
pio device monitor --port COM16 --dtr 0 --rts 0
```

如果 `pio` 不在 PATH 里，用完整路径 `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe`。

> **必须用 `tools\flash.ps1` 烧录，不要用 `pio run -t upload`**：后者在这块板上会报
> `Wrong boot mode detected (0xd)`。原因见 [13.1](#131-烧录必须用-toolsflashps1)。
>
> **看日志必须带 `--dtr 0 --rts 0`**，否则打开串口会让芯片复位或停在下载模式。

**进不了下载模式时**（极少见）：按住 **BOOT** → 点一下 **RESET** → 松开 BOOT，
然后执行 `tools\flash.ps1 -SkipReset`。

### 资源占用

RAM 约 12%（40KB / 320KB），Flash 约 79%（1037KB / 1280KB）。
BLE 协议栈占了大部分 Flash；如果以后功能再加多，需要换更大的应用分区表。

---

## 8. 自定义

### 8.1 用网页改键

`tools/keymap.html` 是一个本地网页，通过 USB 直接读写键盘上的 5 组预设，**不用重新烧录**。

1. 用 USB 线把键盘连到电脑，**关掉** PlatformIO 串口监视器等占用串口的程序。
2. 用**电脑版 Chrome 或 Edge** 双击打开 `tools/keymap.html`（其它浏览器不支持 Web Serial）。
3. 点右上角 **连接键盘**，在弹出的列表里选 `USB JTAG/serial debug unit`（或带 `COMx` 的那一项）。
4. 页面自动读出 5 组预设。选一组，可以修改：
   - **预设名**（≤19 个英文字符）和**类型**（普通快捷键 / 媒体键）
   - 每个键的**修饰键**（Ctrl / Shift / Alt / Win）+ **按键**（最多两个，如 Ctrl+K+C）；
     也可以点 **录制**，然后直接在电脑键盘上按组合键
   - 媒体键：音量、播放 / 暂停、上一曲 / 下一曲、静音、计算器、浏览器后退等
   - **屏幕说明**（≤15 个英文字符，按键时显示在 OLED 上；不填会按组合键自动生成）
5. 点 **写入键盘**：5 组一起写入并保存到 flash，立即生效，断电不丢。

其它按钮：**设为键盘当前预设**（切到正在编辑的这组）、**恢复出厂预设**、**导出到文件 / 从文件导入**（备份或分享键位）。
有改动但还没写入的预设，标签上会有一个圆点。

> - 屏幕字体只有英文字符，预设名和说明里不能用中文。
> - 有些组合键会被浏览器或系统先拦截（如 Ctrl+W、Alt+Tab、Win+D），录制不到；用下拉框手动选即可。
> - 按键码只能用标准键盘区和小键盘（F1~F12 以内），F13 以上键盘发不了。
> - 如果双击打开后点"连接键盘"没反应，可以在 `tools` 目录下运行 `python -m http.server 8000`，
>   再用浏览器打开 `http://localhost:8000/keymap.html`。

### 8.2 在源码里改出厂预设

出厂预设（以及"恢复出厂预设"恢复成的样子）在 `src/settings.cpp` 的 `kDefaultPresets[]`。
每个键是一条 8 字节的 HID 键盘报文：

```
{ 修饰键, 0x00, 键码1, 键码2, 0x00, 0x00, 0x00, 0x00 }
```

| 修饰键（可相加） | 值 |
| --- | --- |
| 左 Ctrl | `0x01` |
| 左 Shift | `0x02` |
| 左 Alt | `0x04` |
| 左 Win / Cmd | `0x08` |

常用键码：`A`~`Z` = `0x04`~`0x1D`，`1`~`9` = `0x1E`~`0x26`，`0` = `0x27`，
Enter `0x28`，Esc `0x29`，Backspace `0x2A`，Tab `0x2B`，空格 `0x2C`，
F1~F12 = `0x3A`~`0x45`，Delete `0x4C`，方向键 → ← ↓ ↑ = `0x4F`~`0x52`。
完整列表见 USB HID Usage Tables 的 Keyboard/Keypad Page (0x07)。

例：Ctrl+Shift+T = `{ 0x03, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00 }`。

每组预设还要填：
- `name`：预设名（≤19 字符，显示在屏幕上）
- `keyDesc`：5 个键的说明（每个 ≤15 字符，按键时显示在屏幕上）
- 全 0 的报文 = 空键

**媒体键**：把预设最后的 `isMedia` 设为 `true`，每个键第 0 字节填媒体键位：
`MEDIA_VOLUME_UP` / `MEDIA_VOLUME_DOWN` / `MEDIA_PLAY_PAUSE` / `MEDIA_MUTE` /
`MEDIA_NEXT_TRACK` / `MEDIA_PREV_TRACK` / `MEDIA_STOP`（定义在 `include/settings.h`）。
一组预设要么全是普通键，要么全是媒体键。

增减预设数量时，同步改 `include/settings.h` 的 `PRESET_COUNT`。

### 8.3 常用参数

| 参数 | 位置 | 默认 |
| --- | --- | --- |
| 灯珠亮度 `LED_BRIGHTNESS`（0~255） | `include/board_config.h` | 96 |
| 蓝牙设备名 `BLE_DEVICE_NAME` | `include/board_config.h` | `ESP32-C3 Keybrick` |
| 是否装了电池 `BATTERY_PRESENT` | `include/board_config.h` | 1 |
| 低电阈值 `BAT_LOW_V` / `BAT_LOW_RELEASE_V` | `include/board_config.h` | 3.50V / 3.60V |
| U4 长按时间 `MODE_HOLD_MS` | `src/system.cpp` | 1000ms |
| 翻页连发延迟 / 间隔 | `include/buttons.h` | 450ms / 140ms |
| 变暗 / 关屏时间 | `src/system.cpp` | 20s / 30s |
| 提示旋律 / 彩蛋曲子 | `src/buzzer.cpp` | 见 [2.3 声音](#23-声音) |
| 电量曲线 `kBatCurve` | `src/battery.cpp` | 见 [5. 电池](#5-电池) |

修改引脚只需要改 `include/board_config.h`，其它文件不涉及具体引脚号。

---

## 9. 串口诊断命令

用 `pio device monitor --port COMx --dtr 0 --rts 0` 打开串口后，**单键输入，不需要回车**。
每次只按一个键：一次收到多个字符（比如粘贴一串、或带回车）会被当成杂讯丢弃，防止误触发。

| 键 | 作用 |
| --- | --- |
| `?` | 显示帮助 |
| `@...` | 网页改键、Claude 状态用的行协议（见 `src/system.cpp` 的"上位机改键协议"注释），手动一般用不到 |
| `s` | 打印状态：模式、预设、电压 / 电量、屏幕、声音档位、灯珠电源 |
| `1`~`5` | 直接切换到第 N 组预设并保存 |
| `p` | 打印当前预设 5 个键的 HID 报文字节 |
| `n` / `c` | 切到 正常 / 预设切换 模式 |
| `v` | 切换"是否装了电池" |
| `e` / `k` | 切换下一个灯效 / 查看当前灯效和 U4 按键状态 |
| `r` | 重跑灯珠自检（红绿蓝白） |
| `x` | 逐颗逐通道检测：每颗灯依次只亮 红 / 绿 / 蓝 / 白 |
| `o` / `f` | 灯珠电源 开 / 关（用万用表量灯珠供电时用） |
| `i` | 扫描 I2C 总线（排查屏幕不亮） |
| `d` | 屏幕探活，并冻结画面显示状态行 + 四角定位点 |
| `F` | 解除 / 恢复屏幕冻结 |
| `P` | 自动模拟 5 个键的按键提示，检查文字位置 |
| `w` | 唤醒屏幕 |
| `g` | 打印 GPIO 矩阵：每个脚当前由哪个外设驱动 |

`g` 输出中 `out_sel` 的含义：`128` = 普通 GPIO 输出，`51` = RMT（灯珠数据），
`45` = LEDC（蜂鸣器），`6` = UART0 TXD。正常时灯珠数据脚（pin1）是 `51`，灯珠电源脚（pin21）是 `128`。

> 启动日志（`=== ESP32-C3 BLE Keybrick booting ===`）只有在复位后**立刻**打开串口才看得到。
> 日志在主机没读时最多等 20ms 就丢弃，这是为了不阻塞按键和灯效。

---

## 10. 故障排查

| 现象 | 排查 |
| --- | --- |
| 烧录报 `Wrong boot mode detected` | 用 `tools\flash.ps1`，不要用 `pio run -t upload`；仍不行就手动进下载模式（见 [7](#7-编译与烧录)） |
| 串口没有任何输出 | 确认带了 `--dtr 0 --rts 0`；复位后马上打开串口；发 `?` 看有没有回应 |
| 搜不到蓝牙设备 | 灯是不是黄色呼吸（在广播）；电脑上删除旧的配对记录后重新搜索 |
| 网页点"连接键盘"列表里没有设备 | 换一根能传数据的 USB 线；确认用的是电脑版 Chrome / Edge |
| 网页提示"打不开串口" | 串口被占用：关掉 PlatformIO 串口监视器、Arduino IDE 等 |
| 网页提示"键盘没有回应" | 固件太旧，先用 `tools\flash.ps1` 烧录最新固件 |
| 能连上但按键没反应 | 是否在正常模式（灯不偏青、屏幕只有状态行）；当前预设的这个键是不是空键（`p` 查看） |
| 灯全不亮 | 串口 `g`：pin21 `out_sel` 应为 `128`；量灯珠供电是否 3.3V（`o`/`f` 切换时应跟着变） |
| 只有第 1 颗灯亮 | LED1 `DO` → LED2 `DI` 之间断开（虚焊或方向） |
| 某颗灯颜色偏黄 / 偏色 | 发 `x` 逐通道检测；某个通道不亮就是那颗灯坏了或虚焊（黄色 = 蓝通道不亮） |
| 红绿颜色反了 | 改 `src/ws2812.cpp` 中 `show()` 的 `order[3] = {1,0,2}` |
| 屏幕不亮 | 串口 `i`：扫到 `0x3C` 说明屏幕在线；什么都扫不到就检查排针 3V3 / GND / SDA / SCL |
| K3 没反应 / 灯全灭 | 检查 `platformio.ini` 中 `ARDUINO_USB_CDC_ON_BOOT=1` 是否还在（见 [13.2](#132-gpio20--gpio21-与-uart0-冲突)） |
| 上电时按键乱触发 / 进了下载模式 | GPIO0 / 2 / 8 / 9 是启动配置脚，上电瞬间不要按住 K1 / K2 / K5 / BOOT |
| 电压和万用表不一致 | 调 `BATTERY_CAL`（见 [5](#5-电池)） |
| 没装电池却显示 4.2V / 100% | 把 `BATTERY_PRESENT` 设为 0，或串口发 `v` |

---

## 11. 硬件与引脚

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| K1 | GPIO0 | 按下接地，内部上拉；启动配置脚 |
| K2 | GPIO2 | 按下接地，板上 10K 上拉；启动配置脚 |
| K3 | GPIO20 | 按下接地，内部上拉；同时是 UART0 RXD 默认脚 |
| K4 | GPIO7 | 按下接地，内部上拉 |
| K5 | GPIO8 | 按下接地，板上 10K 上拉；启动配置脚 |
| U4（灯效 / 模式键） | GPIO6 | 按下接地，内部上拉 |
| WS2812B 数据 | GPIO1 | 经 100Ω 接 LED1，5 颗级联（WS2812B-2020） |
| 灯珠电源开关 | GPIO21 | AO3401 P-MOS 高边开关，**低电平导通**；灯全灭 1 分钟自动断电省电；同时是 UART0 TXD 默认脚 |
| OLED SDA / SCL | GPIO4 / GPIO5 | SSD1306 128×32，硬件 I2C 400kHz，地址 `0x3C` |
| 蜂鸣器 | GPIO10 | 经 S9013 驱动的 2.7kHz 无源蜂鸣器，高电平响 |
| 电池电压 | GPIO3 | BAT+ 经 100K/100K 分压（×0.5），只能作模拟输入 |
| BOOT | GPIO9 | 只用于进下载模式 |
| USB | GPIO18 / GPIO19 | 芯片内置 USB-Serial/JTAG，烧录和日志都走这里 |

电源：TP4056 充电（500mA），TLV75733 LDO 输出 3.3V。
除 GPIO9（BOOT）外，所有可用引脚都已占用。

---

## 12. 代码结构

```
platformio.ini            工程配置
include/board_config.h    所有引脚与硬件参数（改硬件只改这里）
src/main.cpp              启动流程 + 主循环：蓝牙连接、报文发送、各模块调度
src/system.cpp            模式切换、按键与 U4 事件、OLED 界面、串口诊断命令、改键协议
src/buttons.cpp           K1~K5 消抖与连发（非阻塞）
src/settings.cpp          出厂预设表 + 用户预设的 NVS 读写
src/display.cpp           SSD1306 驱动：512 字节帧缓冲、6×8 字体、图标
src/led_effects.cpp       灯效与状态提示的合成，U4 消抖
src/ws2812.cpp            WS2812B 驱动（RMT 硬件时序）+ 灯珠电源开关
src/buzzer.cpp            蜂鸣器：LEDC 硬件 PWM + 非阻塞音符播放器、提示旋律、彩蛋曲子
src/battery.cpp           电池电压采样、电量换算、低电判断
include/usb_log.h         直接写 USB-Serial/JTAG FIFO 的日志
include/display_font.h    6×8 ASCII 字库
include/display_icons.h   电池 / 蓝牙 8×8 图标（取自 u8g2 open-iconic）
lib/hid2ble/              BLE HID 键盘 + 媒体键
tools/flash.ps1           烧录脚本
tools/keymap.html         网页改键工具（Chrome / Edge，Web Serial）
tools/claude_status.py    Claude Code hook：把运行状态推给键盘
tools/install_claude_status.py  一键安装 / 卸载上面这个 hook（任何电脑）
tools/verify_icons.py     图标位图校验脚本
```

所有模块都是非阻塞的：主循环每轮轮询各模块后让出 1ms，不使用硬件定时器中断。

---

## 13. 开发须知：这块板的几个坑

### 13.1 烧录必须用 `tools\flash.ps1`

板子通过芯片内置的 USB-Serial/JTAG 连接电脑。固件运行时，esptool 默认的复位方式进不了下载模式；
而 PlatformIO 总会追加 `--before default_reset` 并覆盖自定义设置。
`flash.ps1` 直接调用 esptool，使用 USB-JTAG 专用的 `--before usb_reset`。

### 13.2 GPIO20 / GPIO21 与 UART0 冲突

K3（GPIO20）和灯珠电源开关（GPIO21）正好是 UART0 的默认 RX / TX。
如果日志走 UART0，这两个脚会被 UART 占用：K3 读不到，灯珠电源无法打开（5 颗灯全灭）。
因此：

- `platformio.ini` 中的 `ARDUINO_USB_MODE=1` 和 `ARDUINO_USB_CDC_ON_BOOT=1` **不能删**，让日志走 USB。
- `ws2812.cpp` 驱动 GPIO21 之前先调用 `gpio_reset_pin()`，把它从 UART0 手里拿回来；**这一句不能删**。

### 13.3 日志不用 `Serial`

arduino-esp32 2.0.x 的 USB CDC（`HWCDC`）在主机没主动发过数据前会丢弃所有输出。
所以日志通过 `include/usb_log.h` 直接写硬件 FIFO，主机不读时最多等 20ms 就放弃。
**不要调用 `Serial.begin()`**，日志统一用 `usbLog::print` / `usbLog::printf`。

### 13.4 OLED 地址是 `0x3C`

Arduino `Wire` 使用 7 位地址。不要写成 8 位形式的 `0x78`（那会在总线上发出 `0xF0`，屏幕不响应）。

### 13.5 I2C 单次传输不能超过 128 字节

`Wire` 的发送缓冲只有 128 字节，超出的部分会被**静默丢弃**（`Wire.write()` 返回 0，不报错）。
`display.cpp` 刷屏时每次只发 1 个控制字节 + 32 个数据字节，并在每帧开始前重设写入窗口，
避免画面错位。修改刷屏代码时要保持这一点。

### 13.6 BLE 栈是异步建立的

`Hid2Ble::begin()` 只是创建一个任务去搭建 BLE 栈。在 `isReady()` 返回 true 之前，
库内部的接口都会直接返回、不会崩溃。断线后重新广播要用 `reAdvertise()`，
**不能再次调用 `begin()`**（会重复初始化 BLE 栈，最终内存耗尽）。

### 13.7 图标位图要用脚本校验

`include/display_icons.h` 里的位图肉眼很难查错。修改后运行：

```powershell
python tools\verify_icons.py
```

脚本会把位图渲染成 ASCII 并检查结构。如果把 u8g2 仓库的
`tools/font/bdf/open_iconic_all_1x.bdf` 放到 `%TEMP%\u8g2m\u8g2-master\tools\font\bdf\`，
还会与官方字形逐像素比对。

### 13.8 硬件改进建议

- **AO3401 栅极加 100K 上拉到源极**：让上电瞬间（GPIO21 还没被固件接管时）灯珠电源确定处于关闭状态。
- **灯珠亮度**：WS2812B-2020 现在由 3.3V 供电，5 颗全白峰值约 175mA。
  想更亮需要改 5V 供电，但数据线高电平门槛会升到 3.5V，需要加电平转换。
- **充电状态检测**：TP4056 的 CHRG 脚目前只接了指示灯。如果接到一个 GPIO，固件就能准确显示"充电中"。

---

## 14. 致谢

- 本项目基于一个开源的 ESP32-C3 蓝牙小键盘工程重写。原工程提供了最初的思路、
  6 组键位预设、6×8 ASCII 字库（`include/display_font.h`）和 BLE HID 库（`lib/hid2ble/`，
  在此基础上修了空指针崩溃、加了断线重连和媒体键）。代码注释里的"原工程"指的就是它。
- 电池和蓝牙图标取自 [u8g2](https://github.com/olikraus/u8g2) 附带的
  [Open Iconic](https://github.com/iconic/open-iconic) 字形。
- 网页改键使用浏览器的 [Web Serial API](https://developer.mozilla.org/docs/Web/API/Web_Serial_API)。

## 15. 许可证

本项目以 [GNU General Public License v3.0](LICENSE) 发布：可以自由使用、修改和再分发，
但基于本项目发布的修改版也必须以 GPL-3.0 开源。
