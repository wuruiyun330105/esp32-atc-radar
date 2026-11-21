# ESP32-S3 塔台雷达桌面站

> 一块微雪 ESP32-S3-Touch-AMOLED-2.16（480×480 / QSPI AMOLED / CST92xx 触摸 / 8MB PSRAM）上的**塔台 PPI 雷达 + 时钟天气**双页桌面站。
> 航班数据可接真实 ADS-B / 飞常准 MCP，点选目标即触发塔台风格语音播报。

---

## 功能

- **塔台 PPI 雷达**：磷光余晖、波束双时间常数衰减、地物杂波、CRT 颗粒、目标闪烁/矢量/尾迹、距离环与方位刻度。12 rpm（5 秒一圈），480×480 满屏，实测 18~20fps。
- **真实航班数据**：多源 ADS-B 合并去重（adsb.fi / adsb.lol / OpenSky），可选本地 dump1090，或飞常准 MCP 拉机场进出港动态。支持死推算外推，刷新间画面不跳帧。
- **点选播报**：点雷达上的航班弹详情卡（ALT / GS / HDG / V/S / DIST / BRG / 机型 / ICAO24），并触发一句塔台口吻的中文播报（数字走民航读法）。
- **时钟天气**：整帧 canvas 重绘，大字时间/温度，和风天气数据源，NTP 校时。
- **配网页**：首次开机无 WiFi 时自动开热点，手机浏览器填 WiFi / 坐标 / 各 API Key，存 NVS，免改代码重烧。
- **PC 模拟器**：UI 与数据层纯 C，PC 上用 SDL2 + LVGL 原地验证，不必先有硬件。

---

## ⚠️ 关于密钥（重要）

**本仓库不含任何 API Key、WiFi 密码或个人录音。** 所有密钥都需要你自己申请、在配网页或 `app_config.json` 里填写。

| 用途 | 服务 | 是否必需 | 说明 |
|---|---|---|---|
| 天气 | 和风天气 dev.qweather.com | 可选 | 免费个人版即可；需同时填 **API Key** 和控制台里的 **专属 API Host** |
| 航班动态 | 飞常准 MCP 开放平台 cmcp.variflight.com | 可选 | Key 形如 `sk-user-...`；按调用计费，固件 10 分钟一刷省额度 |
| 云端语音 | 面壁 ModelBest | 可选 | VoxCPM2 合成；不填则走本地 TTS 后备（见下） |
| ADS-B | adsb.fi / adsb.lol / OpenSky | 可选 | 公益源无需 Key；部分源需邮件申请 |
| WiFi | 你的路由器 | 设备联网必需 | 配网页填写，别写进源码 |

**配置方式（二选一）**

1. **设备端（推荐）**：烧录后手机连热点 → 打开 `http://192.168.4.1` → 填 WiFi 与各项 Key → 保存重启。配置存 NVS，掉电不丢。热点名见 `src/net/web_config.cpp` 里的 `WiFi.softAP(...)`。
2. **PC 端**：把 `app_config.example.json` 复制为 `app_config.json`，填入自己的值。该文件已被 `.gitignore` 忽略。

> 源码 `src/data/data_config.cpp` 的 `cfg_defaults()` 全部是空值兜底，找不到配置时会通过配网页引导填写，不会使用任何内嵌密钥。

---

## 目录结构

```
esp32-atc-radar/
├── CMakeLists.txt            # PC 模拟器构建
├── platformio.ini            # 设备端构建
├── device/lv_conf.h          # 设备端 LVGL 配置
├── sim/lv_conf.h             # 模拟器 LVGL 配置
├── src/
│   ├── main_pc.c             # PC 入口（SDL 显示 + 后台取数线程）
│   ├── main_esp32.cpp        # 设备入口（Arduino setup/loop + 后台任务）
│   ├── app_pages.c/.h        # 双页框架 + 手势切换 + 数据分发
│   ├── ui/                   # 纯 LVGL9，PC/设备共用
│   │   ├── ui_radar.c/.h     #   塔台 PPI
│   │   ├── ui_clock_weather.c/.h  #   时钟天气
│   │   ├── cn_font.c         #   中文字体（lv_font_conv 生成）
│   │   └── font_big128.c     #   大号数字字体
│   ├── data/                 # 数据层：配置/时间/天气/航班/坐标换算
│   ├── net/                  # HTTP(S) / 配网页 / IP 定位 / 语音播报
│   ├── port/                 # 硬件移植层（显示、触摸、音频）
│   └── vendor/miniz/         # 轻量 gzip 解压（和风天气返回 gzip）
├── tools/                    # 本地 TTS / 语音识别服务（Windows 自带语音）
├── scripts/                  # LVGL 下载脚本
├── docs/                     # 烧录 / 排障 / 本地环境指南
└── app_config.example.json   # 配置模板（空值）
```

UI 层与业务层不碰硬件，硬件差异只通过 `port/` 和 `#ifdef ESP_PLATFORM` 隔离，所以 PC 上验证过的界面到设备端基本不用改。

---

## 快速开始

### 烧到板子

```bash
pip install -U platformio
# 1. 拉微雪官方库（提供屏幕/触摸驱动与 pin_config.h）
git clone --depth 1 https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 vendor/waveshare-demo
# 2. 下载 LVGL 9
bash scripts/setup_lvgl.sh          # Windows 用 powershell scripts/setup_lvgl.ps1
# 3. 编译烧录
pio run -t upload
pio device monitor
```

USB-C 直连板子的原生 USB 口。卡在 `Connecting...` 就按住 **BOOT** 再插线。

### PC 上看界面

```bash
# Ubuntu/WSL: sudo apt install cmake build-essential libsdl2-dev
# macOS:      brew install cmake sdl2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/sim
```

- 鼠标拖动 / `←` `→` 切页；雷达页点目标弹详情卡
- `START_PAGE=0/1` 指定启动页（0 雷达 / 1 时钟）
- `RADAR_SRC=0` 强制演示航班，不联网

详细步骤见 [docs/本地打开工程.md](docs/本地打开工程.md) 和 [docs/到货烧录指南.md](docs/到货烧录指南.md)。

---

## 语音播报

`src/net/announce.cpp` 支持两条链路：

1. **云端 VoxCPM2**（面壁）：文案用 MiniCPM5-2B 生成塔台口吻，TTS 合成后经 ES8311 播出。填了 `mbkey` 才启用。
2. **本地 TTS 后备**：`tools/tts_server.py` 用 Windows 自带 System.Speech，零成本离线。设备端填 `tts` = `电脑局域网IP:8765`。

**音色克隆（可选）**：仓库不含任何个人录音。想用自己的声音，录一段 3~10 秒清晰男声 WAV（16k/16bit/单声道），转成 C 数组放进 `src/net/atc_ref_local.h`（已 gitignore），定义 `ATC_REF_WAV` / `ATC_REF_WAV_LEN` / `ATC_REF_TEXT`，再把 `announce.cpp` 的 `#include "atc_ref.h"` 改成 `"atc_ref_local.h"`。默认占位是 0 长度，会自动回退到无克隆合成。

---

## 国内数据实测备注

长三角大陆公益 ADS-B 覆盖较薄（上海 120NM 内常态 0~3 架）。想在上海看到丰富画面，推荐自架 RTL-SDR + dump1090（模式 3），硬件约 200~300 元，1 秒刷新、零配额。详见 `src/data/data_flight_net.c` 注释。

---

## License

第三方依赖（LVGL / miniz / 微雪官方库）各自保留其许可证。本项目自研代码可自由使用与修改，欢迎 PR。
