# ESP32 小说阅读器 / WiFi 直连抓取 — 交接文档

## 1. 项目目标（用户目的）

用户希望基于手上的 **ESP32 2.8 寸屏开发板**完成一个小说阅读器，并且后续要求：

- 在 **ESP32 屏幕上直接填写 WiFi 账号和密码**；
- WiFi 连接后，**由 ESP32 自己直接抓取网页小说内容**；
- 不接受“电脑端先抓取再转发”的方案；
- 提供选书页、九宫格拼音搜索（实现后来改为全键盘 K26，见 4.3），并以《斗罗大陆》作为快捷测试书；
- 章节沿“下一章”渐进发现并支持读到末章，不一次加载整本目录；
- 在线章节永久归档到 TF 卡，并能直接阅读用户放入 TF 卡的 UTF-8 TXT；
- 最终显示在 LVGL 阅读器界面中，支持触摸翻页。

当前阶段已经完成在线抓取、TF 归档和本地 TXT 阅读，后续重点是真机交互与异常场景验证。

## 2. 硬件信息

| 项目 | 信息 |
|---|---|
| 主控 | ESP32-D0WD-V3（经典 ESP32，非 S3） |
| 开发板 | ESP32-2432S028R（Cheap Yellow Display / 2.8 寸） |
| 屏幕 | ILI9341，SPI，物理 240x320，程序横屏使用 320x240 |
| 触摸 | XPT2046，GPIO 时序驱动 |
| TF 卡 | 板载卡槽，SPI3，实测 32GB FAT32 可挂载 |
| Flash | 4MB |
| WiFi | ESP32 内置 2.4G WiFi |
| USB 串口 | CH340（串口号随 USB 重枚举变化，以设备管理器为准） |

### 关键引脚

| 功能 | GPIO |
|---|---|
| LCD SCK | 14 |
| LCD MOSI | 13 |
| LCD MISO | 12 |
| LCD CS | 15 |
| LCD DC | 2 |
| LCD 背光 | 21 |
| 触摸 CLK | 25 |
| 触摸 CS | 33 |
| 触摸 DIN/MOSI | 32 |
| 触摸 OUT/MISO | 39 |
| 触摸 IRQ | 36 |
| TF SCK | 18 |
| TF MOSI | 23 |
| TF MISO | 19 |
| TF CS | 5 |

## 3. 软件架构

```text
ESP-IDF v5.5.4
├── LVGL 9.2
├── ILI9341 硬件 SPI + XPT2046 GPIO 时序
├── SPIFFS + FAT32 TF 卡
├── WiFi STA
├── HTTP + gzip/chunked 网页抓取（搜索/详情支持三镜像自动切换）
└── LVGL WiFi 设置 / 选书搜索 / 阅读器 UI

开机主页包含在线阅读、下载 TXT、本地阅读三个入口；各主要页面都有主页返回按钮。“下载 TXT”通过 TXT80 搜索整本文件，解析两个直连镜像并流式写入 `/sdcard/novels/`；首选镜像失败时自动回退，不复用在线阅读的逐章抓取。
TXT80 同时存在 GBK 和 UTF-8 文件，`gbk_codec.c` 在首个数据块判断编码，并把 GBK 流式转成 UTF-8 后保存；完整 CP936 映射表常驻 Flash，不占用大块运行内存。
```

仓库根目录即工程目录，直接用 ESP-IDF 在该目录下构建。

主要源码：

| 文件 | 作用 |
|---|---|
| `main/main.c` | 显示/触摸/LVGL 初始化，启动 WiFi 设置 UI 与背光 PWM |
| `main/novel_reader.c` | 本地/流式小说阅读器 UI、分页、缺章等待与翻页保护 |
| `main/sd_storage.c` | TF 挂载、书库扫描、在线章节归档、分页索引与阅读断点 |
| `main/touch_bitbang.c` | XPT2046 GPIO 时序驱动，为 TF 释放 SPI3 |
| `main/display_brightness.c` | GPIO21 背光 PWM、10%–100% 亮度控制与 NVS 持久化 |
| `main/wifi_setup_ui.c` | 屏幕上输入 WiFi 名称/密码并连接 |
| `main/wifi_setup_ui.h` | WiFi 设置 UI 接口 |
| `main/book_select_ui.c` | 选书、拼音输入、搜索结果与首章加载 UI |
| `main/web_scraper.c` | 低内存 HTTP 获取（明文，不含 TLS）、gzip 校验解压、分页下载和原子写盘 |
| `main/web_html.c` | 搜索、书籍详情、标题、正文及下一章/同章分页解析 |
| `main/chapter_cache.c` | 5 章滑动缓存窗口计算 |
| `main/fonts/novel_font_16.c` | 自动生成的中文字体（由 OFL 授权字体生成，见 4.2） |
| `legacy/wifi_loader.c` | 早期“ESP32 AP + 电脑中转服务器”原型，**不参与编译**，仅作技术记录 |
| `tools/split_novel.py` | 把本地小说 txt 拆成章节 |
| `tools/build_font.js` | 根据章节字符生成 LVGL 字体 |
| `tools/serve_novel.py` | 电脑端局域网小说服务（已不被用户接受，保留备用；**未纳入仓库**） |
| `tools/fixtures/` | PC 端解析测试用的合成网页样本 |
| `spiffs_data/` | SPIFFS 出厂数据目录（当前为空，小说统一保存到 TF 卡） |

## 4. 当前已完成功能

- 阅读页右上角提供亮度滑杆，拖动实时调光，松开后保存并在重启时恢复；

### 4.1 小说阅读器
- LVGL 显示章节标题、正文、页码；
- 在线模式在 SPIFFS 中只维护阅读位置附近 5 个 `cache_NNN.txt` 章节文件，本地数字章节独立保留；
- 触摸左侧上一页/上一章，其他区域下一页/下一章；
- 翻页保护 0.3 秒，长按不会连翻；
- 章节 ID 索引每章 4 字节，可持续扩展到小说末章。

### 4.2 中文字体
- 使用 OFL 授权的 **Noto Sans SC（Google Fonts 发布的思源黑体，Medium 字重）** 生成 LVGL 自定义字体；
- 当前覆盖 9037 个简体/常用繁体及 UI 字符，并自动收集 `main/*.c` 字符串中的 UI 用字；
- 生成参数为 1bpp、无压缩，避免 LVGL 9 字体兼容问题；
- 源字体路径可用 `FONT_SOURCE` 覆盖，脚本按 Noto Sans SC → 思源黑体顺序自动查找；
- **不得改用 `simhei.ttf` / `msyh.ttc` 等 Windows 随附商业字体**，其许可不允许再分发衍生字形；仓库中的 `main/fonts/novel_font_16.c` 是可直接分发的生成产物；
- 如果新增章节含新字，需要重新运行：
  ```bash
  node tools/build_font.js
  ```

### 4.3 WiFi 设置 UI
- 开机进入 WiFi 设置页面；
- 屏幕显示：
  - WiFi 名称输入框
  - WiFi 密码输入框
  - “连接并选书”与“本地阅读”按钮
  - 彩色状态提示与下载进度条
- 点击输入框时按需创建 LVGL 键盘，关闭后立即释放；
- 连接带三次自动重试，只有成功后才把账号密码保存到 NVS；
- 连接成功后进入选书页；搜索词使用全键盘（K26）拼音输入，结果最多显示 8 条。
- 选书后由 ESP32 直接下载网页；当前章落盘即进入阅读器，其余章节在后台缓存。

### 4.4 触摸映射
- 触摸屏物理分辨率 240x320；
- 当前横屏触摸映射参数：
  ```c
  .x_max   = 240
  .y_max   = 320
  .swap_xy = 1
  .mirror_x = 1
  .mirror_y = 1
  ```
- 用户已反馈“左右颠倒”问题已修复，但仍需继续实机确认整体是否完全对齐。

### 4.5 TF 卡与本地 TXT

- 板载 TF 卡使用 SPI3：SCK=18、MOSI=23、MISO=19、CS=5，挂载点为 `/sdcard`，时钟 10MHz；挂载失败不会自动格式化。
- ESP32 只有两个通用 SPI host，而该板把 LCD、触摸和 TF 分到三组引脚。LCD 保持硬件 SPI2，TF 使用硬件 SPI3，XPT2046 改为 GPIO 时序驱动（CLK=25、MOSI=32、MISO=39、CS=33、IRQ=36）。
- FATFS 已启用堆上长文件名、255 字符上限和 UTF-8 API，可识别中文 TXT 文件名。
- 书架扫描 `/sdcard/*.txt`、`/sdcard/novels/*.txt` 和 `/sdcard/online/<book_id>/`。普通 TXT 按页读取，侧车索引写到 `/sdcard/.reader/`，不整本载入 RAM。
- 在线章节在 SPIFFS 原子发布成功后复制到 `/sdcard/online/<book_id>/<chapter>.txt`，元数据写入 `book.meta`；TF 失败只记录告警，不中断在线阅读。
- TF 普通 TXT 与在线归档分别保存断点。归档允许章节号有空缺，并按真实章节号恢复。

## 5. 在线抓取实现与验收

### 5.1 ESP32 直接抓取网页（已完成）

当前可搜索书源（页面结构和书籍 ID 兼容，运行时自动切换）：
```text
http://www.biqukong.com
http://www.biquguo.com
http://www.biqugeww.com
```

已实现流程：
```text
ESP32 连接 WiFi
   ↓
HTTP GET 搜索或书籍详情
   ↓
gzip 解压
   ↓
从详情页识别引子/序章/第一章，过滤源站混入的前置杂文
   ↓
逐页抓取当前章（含 _2.html / _3.html 分页）并立即追加临时文件
   ↓
原子写入 SPIFFS 后立即进入阅读器
   ↓
沿下一章链接扩展 4 字节 ID 索引，后台维护阅读位置附近 5 章
```

### 5.2 已处理的技术点
1. 目标网站返回 **gzip + chunked HTML**：采用小块增量接收，根据 gzip ISIZE 精确分配输出，并校验 CRC；
2. 网站正文/目录分页：
   - 章节目录按 50 章一页；
   - 正文有时拆成 `xxx_2.html`、`xxx_3.html`；
3. 在线阅读按源码中的 `http://` 地址直连（`web_scraper.c` 的 `BASE_SCHEME_HOST`、`SEARCH_SCHEME_HOST` 均为 http），链路本身不含 TLS 与证书校验；certificate bundle 只在 TLS 连接上生效，目前仅 TXT80 整本下载（`txt_download_ui.c`）使用；
4. HTML 缓冲内原地提取正文，章节文本按实际长度增长，避免固定 24KB/40KB/32KB 缓冲叠加；
5. 章节通过 `.tmp` / `.bak` 两阶段替换，阅读器启动时可恢复中断写入；
6. 阅读器只保存分页偏移和一个单页缓冲，不再为每页分配 1KB。
7. 缓存窗口固定为 5 章：`start = clamp(current - 2)`，靠近首尾时自动贴边；只清理窗口外的 `cache_NNN.txt`，不删除本地数字章节；
8. 阅读器用绝对章号通知后台任务，目标章尚未写完时保留当前页并轮询原子落盘文件。
9. 目录每页 50 章，后台只保留一个目录页的元数据；阅读跨目录页边界时按需获取，因此不再受 20 章上限限制；
10. 正文分页使用 LVGL 字体的实际像素尺寸，正文父容器同时启用裁剪，页码栏不会覆盖正文。
11. 同章多页不再累计在 RAM：每页解析后立刻追加到临时章节文件，完成后原子发布；
12. 单页 gzip 解压结果超过 32KB 时，压缩页短暂经 SPIFFS 暂存，先释放接收块再分配解压块；
13. 搜索与详情请求在三个兼容镜像间轮换重试（`can_failover` 仅对 `SEARCH_SCHEME_HOST` 开头的 URL 生效），规避单个域名连接重置或源站超时；正文与目录请求固定使用主站地址，只做同主机重试。
14. WiFi 成功连接后的意外断线使用 0.5～8 秒退避自动重连；
15. 在线缓存所属书籍写入 SPIFFS，重启后可判断是否复用当前缓存；
16. 阅读位置保存章号和正文 UTF-8 字节偏移，翻页停止两秒后合并写入 NVS；
17. 章节索引在正文发布前持久化，旧版本遗留的“正文已存在、后继 ID 缺失”状态会通过重读目录前沿章节修复。

### 5.3 2026-09-09 真机结果（历史记录）

> 5.3～5.6 是逐次真机验收的原始记录，保留当时描述。其中 5.3 的“一次性保存 20 章”是当时的实现，当前固件固定只保留阅读位置附近的 5 章（见 5.4）；“TLS 证书验证”一句与当前源码不符（当前在线阅读链路为明文 HTTP）。

- CH340 `COM12`，ESP32-D0WD-V3 rev 3.1；
- WiFi/WPA3 连接并取得 IP；
- TLS 证书验证、HTTP 200、gzip CRC 校验全部通过；
- 20/20 章节下载并写入 `/spiffs/001.txt` 至 `020.txt`；
- 下载完成后找到 20 章，打开第 1 章（17 页），触摸翻页后成功进入第 2 章；
- NVS 与 SPIFFS 在“仅烧录应用分区”的迭代期间保留；完整 `idf.py flash` 会按 `CMakeLists.txt:8` 的 `spiffs_create_partition_image` 一并烧录 SPIFFS 镜像，覆盖运行期缓存与目录索引。

### 5.4 5 章流式缓存真机结果
- 应用分区单独烧录后复用已有 NVS 和 20 章 SPIFFS 数据；
- 目录解析完成后立即清理 `006.txt`～`020.txt`，约 0.7 秒后打开第 1 章，缓存稳定为 1～5；
- 翻到第 4 章时先删除 `001.txt`，后台下载并原子写入 `006.txt`，窗口变为 2～6；
- 连续阅读到第 8 章时按位置下载 007～010、逐章清理旧文件，最终窗口为 6～10；
- 下载期间触摸翻页正常，连续最大可用堆块约 45～57KB，无崩溃、看门狗或栈溢出。

### 5.5 全目录与正文安全区真机结果
- 在线目录首页报告 15 个目录页，尾页实取 12 章，设备计算总数为 712 章；
- 阅读器页脚显示总章数 712，后续跨 50 章边界时会按需切换目录页；
- 同一份第 1 章正文由原来的 17 页调整为 21 页，分页按 292×156 像素正文视口计算，最后一行不再进入底部页码区；
- 新固件应用分区已烧录至 CH340 `COM12`，NVS 和现有 5 章缓存保留。

### 5.6 可搜索书源与逐页流水线真机结果（2026-09-09）

- Windows 端真实搜索/详情/章节样本通过 `web_html_selftest` 与 `book_source_probe`；
- 板上主镜像 TLS 被重置时成功自动切换备用镜像并取得 HTTP 200；
- 详情解析跳过源站误混入的《光之子》外篇，从《斗罗大陆》引子开始；
- 首章 3 个网页分页依次请求、解析、追加临时文件，合并后原子发布为 `001.txt`（7550 字节）；
- `001.txt` 完成后立即打开阅读器（21 页），随后后台依次保存 `002.txt`～`005.txt`；
- 五章缓存期间最大连续可用堆块约 36～49KB，无崩溃、看门狗或栈溢出；
- 当前 CH340 因 USB 重枚举从 COM12 变为 COM8，后续以设备管理器实际端口为准。

## 6. 构建与烧录

本项目在 **ESP-IDF v5.5.4** 上开发验证。先按官方文档安装 IDF，然后在已执行 `export` 的终端里操作：

Windows（PowerShell/CMD）：
```powershell
%IDF_PATH%\export.bat
```

Linux / macOS：
```bash
. $IDF_PATH/export.sh
```

构建/烧录：
```bash
idf.py build
idf.py -p <PORT> flash monitor
```

注意：CH340 串口号会随 USB 重枚举变化（本机曾出现 COM8 / COM12 互换），烧录前先用设备管理器或 `ls /dev/ttyUSB*` 确认当前端口。

## 7. 重要备注

- 用户明确要求 **不要在电脑端做抓取中转**，所有抓取/解析必须由 ESP32 完成；
- 若后续直接抓取遇到反爬，需要把错误显示在屏幕上，而不是静默失败；
- 当前 `tools/serve_novel.py` 仅作为技术参考，不再作为主推方案；
- 目标站结构或反爬策略未来变化时，应优先更新 `web_html.c` fixture 与解析测试。

## 8. 文档与代码一致性

本文件与 `README.md` 描述的是**当前实现**；带“（历史记录）”标注的小节只代表当时那一次烧录，其中的数字与结论不作为现状依据。本次整理已修正的偏差：

| 位置 | 原描述 | 实际情况 |
|---|---|---|
| 5.2 第 3 条 | “HTTPS 使用 certificate bundle 完成证书校验” | 在线阅读链路为明文 HTTP；certificate bundle 仅 TXT80 下载链路使用 |
| 5.2 第 13 条 | “三个兼容镜像轮换重试” | 仅搜索/详情请求参与镜像切换；正文与目录固定主站、同主机重试 |
| 4.3 | “九宫格拼音输入” | 代码使用全键盘 `LV_IME_PINYIN_MODE_K26` |
| 5.3 | “NVS 与 SPIFFS 均保留” | 仅适用于单独烧录应用分区；完整 `idf.py flash` 会覆盖 SPIFFS |

配置与代码脱节、改动功能时需一并处理：

- `sdkconfig.defaults:23` 的 `CONFIG_LV_MEM_CUSTOM=y` 在 LVGL 9.2 的 Kconfig 中不存在，不会生效；实际生效的是 `CONFIG_LV_USE_BUILTIN_MALLOC` + `CONFIG_LV_MEM_SIZE_KILOBYTES=64`（固定池、不可扩展）。
- `CONFIG_LV_IME_PINYIN_USE_K9_MODE=y` 仍然开启，但代码只用 K26 模式，属残留配置。
- `CONFIG_LV_USE_PERF_MONITOR` / `CONFIG_LV_USE_SYSMON` 已开启，代码中没有使用点。

## 9. 开源整理（2026-09-13）

仓库已初始化 Git 并整理为可公开发布状态：

| 项目 | 处理 |
|---|---|
| 生成字体 | 源字体由 `simhei.ttf` 换为 **Noto Sans SC Medium（OFL）**，重新生成 `main/fonts/novel_font_16.c`（9037 字，2,627,146 字节）；原 SimHei 版本可在 `f676b6d` 中找回 |
| 网页样本 | 真实抓取的 `index.html` / `chap.html` / `list.html` / `txt*.html` 等全部不纳入版本管理，测试改用 `tools/fixtures/` 下的合成样本 |
| `wifi_loader` | 已废弃的“AP + 电脑中转”原型移入 `legacy/`，不参与编译 |
| 参赛材料 | `deliverables/`、`.qa/`、`tools/serve_novel.py` 由 `.gitignore` 排除 |
| 授权 | 新增 `LICENSE`（MIT）与 README 的授权/免责说明 |

发布前仍待确认：

- `dependencies.lock` 与 `main/idf_component.yml` 的组件版本需与实际构建一致；
- 无 `build/source_*.html` 时 `book_source_probe` 需改用 `tools/fixtures/` 样本或自行抓取（工具本身不依赖固定文件名）。
