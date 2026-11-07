# Orbis 控制屏 · Home Assistant 插件（add-on）

在 HA 网页里可视化编辑 JC3636W518 控制屏的 JSON 布局，保存后写到
`/config/www/orbis/layout.json`，屏幕开机时从这个地址读取布局。

```
orbis-panel/
├── repository.yaml              # add-on 仓库描述（把这个目录作为仓库添加）
└── orbis_panel/
    ├── config.yaml              # 插件元数据（ingress 面板、映射 /config）
    ├── build.yaml               # 三种架构的基础镜像
    ├── Dockerfile
    └── app/
        ├── main.py              # Python 标准库：读/写 layout.json，代理 HA 实体列表
        └── index.html           # 编辑器（页面 tab + 卡片 + 实体搜索）
```

## 安装

### 方式 A：作为 add-on 仓库（推荐，需要能访问 GitHub）

1. 把这个仓库推到 GitHub（`ha_addon/repository.yaml` 要在仓库根，或直接把 `ha_addon/` 单独推一个仓库）；
2. HA → **设置 → 加载项（Add-ons）→ 加载项商店 → 右上角 ⋮ → 仓库**，
   添加 `https://github.com/<你>/<仓库>`；
3. 商店里出现 **Orbis Panel Editor**，安装、启动；
4. 左侧栏出现 **Orbis 控制屏** 面板。

### 方式 B：不用 GitHub（本地插件）

1. 用 Samba / File Editor 把 `orbis_panel/` 整个目录拷到 HA 的 `/addons/` 下；
2. HA → 设置 → 加载项 → 右上角 ⋮ → **检查更新**（或重启 HA），
   商店最下面"本地加载项"里出现 **Orbis Panel Editor**；
3. 安装、启动，左侧栏出现面板。

### 不是 HA OS / Supervised（Home Assistant Container）

没有 add-on 机制，直接用 File Editor / Samba 编辑
`/config/www/orbis/layout.json` 即可，格式见 [`../docs/layout.example.json`](../docs/layout.example.json)。
也可以在本机跑这个 Python 标准库 应用（只依赖 Python 标准库，设置 `LAYOUT_PATH` 即可），
但 `SUPERVISOR_TOKEN` 在容器外拿不到，实体列表功能会不可用（布局仍可编辑）。

## 用法

- 顶部标签页 = 屏幕上的页（每个屏幕最多 6 页，每页 4 个卡片，2×2）；
- 右侧搜索框找实体，点一下加到当前页；
- 点卡片改标题，双击标签页改名，**+ 新页** 增加一页；
- **保存并发布** → 写入 `layout.json`（version 自增）；
- 屏幕**下次开机**读取新布局（固件目前没有热重载）。

## 布局 JSON 格式

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

- `type` 可省略，按域名推断：`sensor.*` → `sensor`；`script.*` / `scene.*` / `automation.*` → `scene`；其它 → `toggle`；
- `label` 可省略，用 HA 的 `friendly_name`；
- 每页最多 4 个 tile（固件限制），最多 6 页。

## 安全说明

- 插件只读写 `/config/www/orbis/layout.json`，不接触 HA 的配置或数据库；
- 访问 HA 实体列表用的是 Supervisor 注入的 `SUPERVISOR_TOKEN`，令牌不落盘；
- 屏幕固件读取的是 `/local/orbis/layout.json`，这是 HA 的公开静态目录，
  **不要把敏感信息写进布局文件**。
