# Orbis Panel · JC3636W518 圆屏 Home Assistant 控制屏

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.4-red.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Target](https://img.shields.io/badge/target-ESP32--S3-8A2BE2.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Home Assistant Add-on](https://img.shields.io/badge/Home%20Assistant-add--on-41BDF5.svg)](https://github.com/xiao-si-qi/orbis-panel)

> 硬件：GUITION **太极小派 JC3636W518**（2024-07 批次）。本项目不是厂家官方项目。

基于 **ESP32-S3R8 + 1.85" 圆形 360×360 ST77916** 模组的 Home Assistant 触屏控制面板。

> **开发方式：本项目全部由 AI 完成。** 固件、HA 插件、字体生成脚本和文档都是 AI（DeepSeek 助手）
> 与使用者结对产出：需求拆解、驱动移植、LVGL UI、JSON 布局协议、实体挑选策略、踩坑排查、发布流程。
> 人类负责硬件选型、实机验证和验收。代码按"能跑、能复现"的标准交付，但不承诺工业级质量，
> 用于家电控制前请自行评估风险。

点卡片直接调用 HA 服务（`light.toggle` / `switch.toggle` / `fan.toggle` …），状态每 5 秒轮询刷新。

```
┌──────────────── 360 x 360 圆屏 ────────────────┐
│              Home            ●WiFi  ●HA       │
│   ┌──────────────┐  ┌──────────────┐          │
│   │ 大厅吊灯      │  │ 阳台灯        │          │
│   │ on           │  │ off          │          │
│   └──────────────┘  └──────────────┘          │
│   ┌──────────────┐  ┌──────────────┐          │
│   │ 风扇          │  │ 场景·回家     │          │
│   │ off          │  │ on           │          │
│   └──────────────┘  └──────────────┘          │
└────────────────────────────────────────────────┘
```

## 硬件

| 项目 | 参数 |
|---|---|
| MCU | ESP32-S3R8，双核 240MHz，8MB Octal PSRAM，16MB Flash |
| 屏幕 | 1.85" 圆屏 360×360 IPS，ST77916，**4-line QSPI @40MHz** |
| 触摸 | CST816S 单点电容触摸，I2C 400kHz（SDA 7 / SCL 8 / RST 40 / INT 41） |
| 背光 | GPIO15，LEDC PWM 5kHz 调光 |
| 其他 | TF 卡（SDMMC 4-bit）、PCM5100A I2S DAC、I2S 麦克风、QI 无线充 |

引脚定义集中在 [`main/bsp_board.h`](main/bsp_board.h)，与厂家 `pincfg.h` 一致。

### 硬件批次（重要）

这块板是太极小派的 **2024-07 批次**（标签批次号 ≤ 2528），属于"早期版本"：

| 项目 | 2024-07 批次（本板） | 2025-07 之后（批次 > 2528） |
|---|---|---|
| 麦克风 | **标准 I2S**：SCK=42 / WS=45 / SD=46 | **PDM**：CLK=45 / DIN=46（无 SCK） |
| 屏幕玻璃 | 早期玻璃 | 换过玻璃，可能需重新调镜像/颜色 |
| 厂商例程 | `Demo_Arduino`（V1，含 `MIC_I2S_SCK 42`） | `Demo_Arduino_V2.0`（V2，`MIC_I2S_SCK` 被注释） |
| 小智固件预设 | `taiji-pi-s3`（I2S Type STD） | `taiji-pi-s3-pdm`（I2S Type PDM） |

判断依据：厂家资料包里 V2.0 例程把 `MIC_I2S_SCK` 注释掉了（PDM 没有 SCK），而 V1 例程保留了 GPIO42。
本工程目前不涉及音频；将来加语音输入时按 **I2S STD（42/45/46）** 走。

## 目录结构

```
orbis-panel/
├── CMakeLists.txt
├── partitions.csv              # 16MB: app 4MB + assets(FAT) 11MB
├── sdkconfig.defaults          # 16MB QIO / Octal PSRAM 80M / LVGL 9 配置
├── components/
│   ├── esp_lcd_st77916/        # ST77916 QSPI 面板驱动（含厂家初始化序列）
│   └── cst816s/                # CST816S 触摸驱动
├── main/
│   ├── main.c                  # 启动流程
│   ├── bsp_display.c           # QSPI + 面板 + 背光
│   ├── bsp_touch.c             # I2C + 触摸
│   ├── bsp_lvgl.c              # LVGL 9 移植（flush / indev / tick）
│   ├── layout.c                # JSON 布局：从 HA 拉取 / 兜底自动生成
│   ├── ui_home.c               # 界面：完全由布局驱动
│   ├── ha_client.c             # HA REST 客户端（keep-alive + 轮询 + 服务调用）
│   ├── wifi_manager.c          # STA 连接 / 退避重连 / 配网热点
│   ├── app_settings.c          # NVS 配置（Wi-Fi、HA 地址、令牌）
│   ├── fonts/                  # 生成的 Noto Sans SC 中文字体（见 docs/FONT.md）
│   ├── secrets.h               # 出厂默认值（已在 .gitignore，勿提交）
│   └── Kconfig.projbuild       # menuconfig: 轮询间隔、实体槽位
├── ha_addon/                   # HA 插件：网页里编辑布局 JSON
│   ├── repository.yaml
│   └── orbis_panel/
├── docs/
│   ├── layout.example.json     # 布局示例
│   └── FONT.md                 # 字库来源/许可/重新生成
├── tools/gen_cjk_font.py       # TTF → LVGL 字体生成脚本
└── LICENSE                     # MIT
```

## 编译 & 烧录

用 **ESP-IDF v5.5.4**（与本机其它工程一致）：

```powershell
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
cd C:\path\to\orbis-panel

idf.py set-target esp32s3      # 只需一次
idf.py build
idf.py -p COM8 flash monitor
```

LVGL 通过组件管理器解析（本机组件仓库里有 9.2.x，可离线）。首次构建会生成
`dependencies.lock` 和 `managed_components/`。

## 首次配网（设置页）

出厂默认值写在 `main/secrets.h`：

```c
#define APP_SECRET_WIFI_SSID      "your-ssid"
#define APP_SECRET_WIFI_PASSWORD  "your-password"
#define APP_SECRET_HA_URL         "https://ha.example.com"
#define APP_SECRET_HA_TOKEN       "eyJhbGciOi..."
```

首次开机时会播种进 NVS。之后要改配置，两种办法：

1. **配网页面**：设备连不上 Wi-Fi 时会开热点 `Orbis-Setup`，
   手机连上后打开 <http://192.168.4.1>，填 SSID / 密码 / HA 地址 / 令牌，保存后自动重启。
2. **菜单配置**：`idf.py menuconfig` → `Orbis panel`，可指定 4 个实体槽位
   （`light.xxx` 这类 entity_id），留空则自动从 `/api/states` 里挑前 4 个可控实体。

## Home Assistant 端准备

1. HA → 左下角用户名 → 「安全」→ 「长期访问令牌」→ 创建令牌（已有则复用）。
2. 确认地址可达：`https://ha.example.com/api/`，设备用 `Authorization: Bearer <token>`。
3. 用的 REST 接口：
   - `GET  /api/states`                实体发现
   - `GET  /api/states/<entity_id>`    取 `state`
   - `POST /api/services/<domain>/toggle`  `{"entity_id":"..."}`

> 令牌是明文存放在 `secrets.h` / NVS 里的，建议在 HA 里单独建一个只用于这块屏的令牌，
> 万一板子丢了直接吊销这一个即可。

## 布局：JSON 驱动（改布局不用重烧固件）

开机时固件会先尝试从 HA 拉布局：

```
<ha_url>/local/orbis/layout.json
```

（对应 HA 的 `config/www/orbis/layout.json`，`/local/` 是公开静态目录，不需要令牌。）

```json
{
  "version": 3,
  "title": "Home",
  "pages": [
    {
      "name": "客厅",
      "tiles": [
        {"entity": "light.living_room", "label": "客厅灯", "type": "toggle"},
        {"entity": "sensor.living_temp", "label": "温度", "type": "sensor"}
      ]
    }
  ]
}
```

规则：

- 每页最多 **4 个卡片**（2×2），最多 **6 页**；页面多时底部出现翻页圆点，也支持左右滑动；
- `type`：`toggle`（点一下调 `toggle`）/ `scene`（调 `turn_on`）/ `sensor`（只读，显示值+单位）；
  省略时按域名推断（`sensor.*` → sensor，`script./scene./automation.` → scene，其它 → toggle）；
- `label` 省略时用 HA 的 `friendly_name`；
- 启动时**一次** `/api/states` 把布局里所有实体的名字/状态/单位填好，之后只轮询这些实体；
- **拉不到布局文件**（404 / 离线）时退回自动挑选：灯 → 风扇/布尔/开关 → 场景 → 传感器；
- 改完布局**下次开机生效**（目前没有热重载）。

`ha_addon/` 里有一个 Home Assistant 插件（**Orbis Panel Editor**），
在 HA 网页里用「页面 tab + 卡片 + 实体搜索」编辑这个 JSON 并一键发布到
`/config/www/orbis/layout.json`，不需要手改文件。安装见 [ha_addon/README.md](ha_addon/README.md)。

## 已实现 / 待办

- [x] ST77916 QSPI 显示 + 背光 PWM + LVGL 9
- [x] CST816S 触摸 + 卡片点击
- [x] Wi-Fi STA + 退避重连 + 配网页面
- [x] HA REST 轮询 + 服务调用（异步，不卡 UI）
- [x] JSON 布局（页面 / 卡片 / 实体全由 HA 上的 JSON 决定）
- [x] HA 插件：网页端可视化编辑并发布布局
- [x] GB2312 中文字体（Noto Sans SC，OFL）
- [ ] 时间显示（SNTP）
- [ ] 布局热重载（现在要重启才生效）
- [ ] WebSocket 订阅 `state_changed`，替代轮询
- [ ] TF 卡壁纸/音效（SDMMC 引脚已在 `bsp_board.h` 定义）


## 恢复原厂固件

原厂 16MB 整片镜像与恢复脚本在：

```
backups\jc3636w518\restore_firmware.ps1
backups\jc3636w518\MANIFEST.md
```

本工程分区表（`partitions.csv`）与原厂不同，恢复原厂固件会整片覆盖回出厂状态。

## 备注

- 屏幕方向/镜像：`bsp_display.c` 里 `esp_lcd_panel_invert_color(true)`，
  若颜色反了改成 `false`；上下/左右反了在 `esp_lcd_panel_mirror()` 调。
- QSPI 时钟默认 40MHz（稳），想更快把 `BSP_LCD_PIXEL_CLOCK_HZ` 改 50MHz，
  与厂家 demo 一致；花屏就退回来。
- LVGL 缓冲两片各 28.8KB（内部 DMA RAM），全屏双缓冲方案需要放 PSRAM，
  当前 40 行分块已足够 30fps 的卡片界面。

## 开源与许可

- 代码：**MIT**（见 [LICENSE](LICENSE)）。
- 中文字体：由 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)（**SIL OFL 1.1**）
  栅格化成位图嵌入，字体来源与重新生成方式见 [docs/FONT.md](docs/FONT.md)。
- LVGL 9.2、ESP-IDF 5.5.4 各自的许可证见其上游仓库。
- **发布前务必检查**：

| 不要提交 | 原因 |
|---|---|
| `main/secrets.h` | 含 Wi‑Fi 密码与 HA 长期令牌（已在 `.gitignore`，模板见 `secrets.h.example`）|
| `sdkconfig` | 可能包含令牌/路径等本地信息 |
| `build/`、`managed_components/` | 生成物 |
| `backups/jc3636w518/*.bin`、`JC3636W518CN/` 里的厂家资料 | 厂家固件/PDF 版权，不要随仓库分发 |
| 用 Microsoft YaHei 生成的旧字体 | 该字体不可再分发，已换成 Noto |

- 仓库说明建议写明：这是一块 **GUITION 太极小派 JC3636W518**（2024‑07 批次，ST77916 + CST816S）的
  Home Assistant 控制屏固件，不是厂家官方项目。

