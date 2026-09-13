# ESP32 Pocket Reader

> 📖 A lightweight online & offline e-book reader for ESP32-2432S028R, built with ESP-IDF and LVGL.

一个运行在 **ESP32-2432S028R（Cheap Yellow Display）** 上的便携式小说阅读器。

项目基于 **ESP-IDF + LVGL** 开发，在经典 ESP32、4MB Flash 和有限 RAM 的资源约束下，实现了在线搜索、网页章节抓取、拼音输入、流式缓存、TF 卡书架、TXT 下载、断点续读以及触摸阅读等功能。

整个在线阅读流程均由 ESP32 独立完成，不依赖电脑作为内容中转服务器。

---

## ✨ Features

### 在线阅读

* ESP32 直接通过 WiFi 请求网页内容，不经过电脑中转
* 支持按书名 / 作者搜索
* 支持中文拼音输入与候选词
* 自动解析书籍详情、章节正文和下一章链接
* 支持章节多网页合并
* HTTP / HTTPS 双链路：在线阅读走书源提供的 HTTP 入口，TXT 下载链路使用 HTTPS
* 支持 gzip 压缩响应与 chunked 传输编码
* 搜索与详情请求支持多镜像自动切换
* WiFi 意外断开后自动重连（0.5～8 秒退避）
* 当前章节下载完成后立即进入阅读，无需等待整本小说下载

### 低内存章节缓存

在线阅读采用 **5 章滑动缓存窗口**：

```text
当前位置：Chapter 1
缓存：1 2 3 4 5

当前位置：Chapter 4
缓存：2 3 4 5 6

当前位置：Chapter 8
缓存：6 7 8 9 10
```

ESP32 只保存阅读位置附近的章节，自动清理窗口之外的缓存文件，从而避免长篇小说耗尽 Flash。

章节索引仅保存章节 ID（每章 4 字节），不需要一次性载入完整目录。

---

## 📚 TF 卡书架

支持从 TF 卡直接读取小说：

```text
/sdcard/
├── book1.txt
├── book2.txt
│
├── novels/
│   ├── novel_a.txt
│   └── novel_b.txt
│
├── online/
│   └── <book_id>/
│       ├── book.meta
│       ├── 001.txt
│       ├── 002.txt
│       └── ...
│
└── .reader/
    └── ...
```

支持：

* UTF-8 TXT
* 中文文件名
* 长篇 TXT 按需读取
* 分页索引
* 阅读百分比
* 阅读断点
* 在线章节永久归档
* 在线小说离线继续阅读

小说不会被整本加载到 RAM。把 TXT 文件拷到卡的根目录或 `novels/` 文件夹即可，设备不会自动格式化无法识别的卡。

---

## ⬇️ TXT 下载

除逐章在线阅读外，项目还提供独立的完整 TXT 下载功能。

下载过程采用流式处理：

```text
HTTP Response
      │
      ▼
 Encoding Detection
      │
      ├── UTF-8 ───────────┐
      │                    │
      └── GBK → UTF-8      │
                           ▼
                     TF Card
```

支持自动识别：

* UTF-8
* GBK / CP936

GBK 文件会在下载过程中分块转换为 UTF-8，不需要把整本小说加载进内存。

---

## 🧠 Memory-Constrained Design

这个项目的主要目标之一，是探索在资源有限的经典 ESP32 上实现完整阅读应用。

主要优化包括：

### 5 章滑动缓存

只保存当前阅读位置附近的章节，窗口外的缓存文件自动清理。

### 流式网页处理

章节被拆成多个网页时：

```text
Download Page
     ↓
Decompress
     ↓
Parse Body
     ↓
Append Temporary File
     ↓
Release RAM
     ↓
Download Next Page
```

不会同时在 RAM 中保存整章的所有网页。

### 大 gzip 页面处理

当解压结果可能超过 ESP32 可用连续堆块时：

```text
Compressed HTTP Data
        ↓
   Temporary SPIFFS
        ↓
 Release Receive Buffer
        ↓
 Allocate Inflate Buffer
        ↓
      Inflate
```

降低大连续内存分配失败的概率。

### 原子章节发布

章节下载流程使用临时文件，只有整章（含所有分页）完成才发布：

```text
chapter.tmp
    ↓
download / parse
    ↓
complete
    ↓
atomic replace
    ↓
chapter.txt
```

避免阅读器打开只下载了一半的章节。中断写入留下的 `.tmp` / `.bak` 会在下次启动时恢复或清理。

---

## 🖥️ Reader

阅读器支持：

* 中文字体
* 章节标题
* 页码
* 正文分页
* 上一页 / 下一页
* 上一章 / 下一章（向前跨章定位到上一章末页）
* 阅读位置恢复
* 阅读百分比
* 屏幕亮度调整
* NVS 持久化
* 异步等待后台章节下载

阅读位置使用正文 UTF-8 字节偏移保存，而不是简单记录页码，因此重新分页后仍具有较好的恢复能力。

---

## 🀄 Chinese Font

项目使用 LVGL 自定义 1-bit 中文字体（16 px，字形存储在 Flash 中，不占用大块运行时 RAM），当前包含约 **9000+** 个简体中文、常用繁体中文及 UI 字符。

字体由 OFL 授权字体生成，生成脚本会自动收集 `main/*.c` 中的 UI 字符串与章节正文用字：

```bash
npm install
node tools/build_font.js
```

> 字体来源可用 `FONT_SOURCE` 环境变量指定，默认依次查找 Noto Sans SC / 思源黑体。
> **请勿使用 simhei.ttf、msyh.ttc 等 Windows 随附商业字体**——其许可不允许再分发衍生字形。

---

## ⌨️ Chinese Input

在线搜索界面包含拼音输入系统：

```text
Keyboard
   ↓
Pinyin
   ↓
Dictionary
   ↓
Candidate Characters
   ↓
Search Query
```

支持候选词选择，并针对 ESP32 的 RAM 限制控制输入法字典和 UI 对象占用。软键盘按需创建，关闭后立即释放。

---

## 🔧 Hardware

主要测试硬件：

| Component        | Model           |
| ---------------- | --------------- |
| Board            | ESP32-2432S028R |
| MCU              | ESP32-D0WD-V3   |
| Display          | ILI9341         |
| Resolution       | 320 × 240       |
| Touch            | XPT2046         |
| Storage          | 4MB Flash       |
| External Storage | microSD / TF    |
| Network          | 2.4GHz WiFi     |

### Pin Configuration

| Function   | GPIO |
| ---------- | ---: |
| LCD SCK    |   14 |
| LCD MOSI   |   13 |
| LCD MISO   |   12 |
| LCD CS     |   15 |
| LCD DC     |    2 |
| Backlight  |   21 |
| Touch CLK  |   25 |
| Touch CS   |   33 |
| Touch MOSI |   32 |
| Touch MISO |   39 |
| Touch IRQ  |   36 |
| TF SCK     |   18 |
| TF MOSI    |   23 |
| TF MISO    |   19 |
| TF CS      |    5 |

由于该开发板的 LCD、XPT2046 和 TF 卡使用三组不同 SPI 引脚，而经典 ESP32 可用通用硬件 SPI Host 数量有限，因此：

```text
LCD      → Hardware SPI
TF Card  → Hardware SPI
XPT2046  → GPIO Bit-Bang SPI
```

以避免触摸控制器占用 TF 卡所需的 SPI Host。

---

## 🏗️ Software Architecture

```text
                     ┌───────────────────┐
                     │      LVGL UI      │
                     └─────────┬─────────┘
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
          ▼                    ▼                    ▼
   Book Selection        Local Bookshelf       TXT Download
          │                    │                    │
          ▼                    ▼                    ▼
    Web Scraper           SD Storage           GBK Codec
          │                    │
          ▼                    │
      Web HTML                 │
          │                    │
          ▼                    ▼
    Chapter Cache ─────── Novel Reader
          │
          ▼
       SPIFFS
```

主要模块：

| Module                 | Description                      |
| ---------------------- | -------------------------------- |
| `main.c`               | LCD、触摸、LVGL 与系统初始化                |
| `main_menu_ui.c`       | 主菜单                              |
| `wifi_setup_ui.c`      | WiFi 配置和连接管理                     |
| `book_select_ui.c`     | 搜书、拼音输入和在线书籍选择                   |
| `web_scraper.c`        | HTTP/HTTPS、gzip、缓存和章节下载           |
| `web_html.c`           | HTML 搜索结果、正文和章节链接解析              |
| `chapter_cache.c`      | 5 章滑动缓存算法                        |
| `novel_reader.c`       | 小说分页和阅读器                         |
| `sd_storage.c`         | TF 卡、在线归档、TXT 分页和断点               |
| `txt_download_ui.c`    | 完整 TXT 搜索和下载                     |
| `gbk_codec.c`          | GBK / CP936 → UTF-8              |
| `pinyin_dict.c`        | 拼音输入字典                           |
| `touch_bitbang.c`      | XPT2046 GPIO SPI                 |
| `display_brightness.c` | 背光 PWM 与亮度持久化                    |

---

## 📦 Requirements

推荐环境：

```text
ESP-IDF >= 5.4
LVGL 9.2.x
Node.js       # only required when rebuilding fonts
Python 3      # helper tools
```

项目组件依赖由 ESP-IDF Component Manager 管理：

```yaml
idf: ">=5.4"
lvgl/lvgl: "~9.2.0"
espressif/esp_lcd_ili9341: "^1.0"
```

`dependencies.lock` 锁定了实际使用的组件版本与哈希。

---

## 🚀 Build

首先安装并配置 ESP-IDF。

克隆项目：

```bash
git clone https://github.com/<your-name>/ESP32-Pocket-Reader.git
cd ESP32-Pocket-Reader
```

设置目标：

```bash
idf.py set-target esp32
```

编译：

```bash
idf.py build
```

烧录并监视：

```bash
idf.py -p <PORT> flash monitor
```

> 端口号以设备管理器实际显示为准（Windows 上是 `COMx`）。
> 注意：完整 `idf.py flash` 会一并烧录 SPIFFS 镜像，覆盖设备上运行期的章节缓存与目录索引。

---

## 💾 Partition Table

默认针对 4MB Flash：

```text
NVS       24 KB
PHY        4 KB
APP        3 MB
SPIFFS   960 KB
```

```csv
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000
spiffs,   data, spiffs,  0x310000, 0x0F0000
```

---

## 🧪 Tests

与硬件无关的模块支持直接在 PC 上测试。以下命令请在**仓库根目录**执行。

### Chapter Cache

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Imain \
    tools/chapter_cache_selftest.c main/chapter_cache.c \
    -o chapter_cache_selftest
./chapter_cache_selftest
```

预期输出：

```text
CHAPTER CACHE SELFTEST PASSED
```

### HTML Parser

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic \
    tools/web_html_selftest.c main/web_html.c \
    -o web_html_selftest
./web_html_selftest
```

预期输出：

```text
SELFTEST PASSED
```

测试使用 `tools/fixtures/` 下**自行构造的最小合成页面**，以及大量内联合成用例（实体解码、链接去重、分页顺序与循环保护等），仓库不包含任何抓取到的第三方页面或小说正文。

---

## 🛠️ Utility Tools

```text
tools/
├── build_font.js
├── build_pinyin_dict.py
├── split_novel.py
├── chapter_cache_selftest.c
├── web_html_selftest.c
├── book_source_probe.c
└── fixtures/
```

### Split TXT into chapters

```bash
python tools/split_novel.py novel.txt spiffs_data
```

### Rebuild Chinese font

```bash
npm install
node tools/build_font.js
```

### Probe a book source

`book_source_probe` 用于离线验证解析器：先用浏览器保存搜索页、详情页和章节页，再传给该工具。

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic \
    tools/book_source_probe.c main/web_html.c -o book_source_probe
./book_source_probe search.html detail.html chapter.html
```

---

## ⚠️ Limitations

### Upstream websites

在线解析器依赖第三方网页 HTML 结构。

网站修改页面结构、增加验证码、反爬策略或关闭服务后，对应解析器可能失效，表现为搜索无结果或章节加载失败。

### RAM

项目运行于经典 ESP32，连续可用堆空间有限，因此新增网络缓存和 UI 功能时需要特别关注内存碎片。

### Flash

默认目标只有 4MB Flash，因此 SPIFFS 被设计为临时在线缓存，而不是整本小说存储空间。

长期内容推荐保存在 TF 卡。

### WiFi Credentials

设备会把用户输入的 WiFi SSID 和密码保存到 **NVS**，以便下次开机自动连接。

凭据以明文形式存放在 NVS 分区中，不参与固件镜像。如果项目用于安全要求较高的设备，建议进一步启用 NVS Encryption / Flash Encryption 等安全机制。

---

## 🔒 Privacy

项目不需要云端账号，也不包含作者本人的 WiFi 密码、API Key 或 Token。

网络请求由 ESP32 设备直接发起。

---

## 📖 Content & Copyright

本项目是用于学习 **ESP32、LVGL、网络协议、低内存数据处理及嵌入式存储设计** 的技术项目。

仓库本身不提供或分发小说正文，也不包含任何抓取到的页面快照。测试只使用自行构造的合成 fixture。

通过本项目访问、下载或保存的第三方内容，其版权归原作者及相应权利人所有。用户应自行确保其内容来源和使用方式符合当地法律及相关网站的使用条款。

---

## 🗺️ Roadmap

* [ ] 更多可插拔书源
* [ ] 阅读字号调整
* [ ] 字体切换
* [ ] EPUB 支持
* [ ] 阅读主题 / 夜间模式
* [ ] 更完善的书架管理
* [ ] 网络层与书源解析器进一步解耦
* [ ] 自动化测试与 GitHub Actions CI
* [ ] 长时间运行与断电故障测试

---

## 📄 License

This project is licensed under the MIT License.

See [LICENSE](LICENSE) for details.

第三方组件（ESP-IDF、LVGL、esp_lcd_ili9341 等）遵循各自协议；`main/gbk_codec.c` 与 `main/pinyin_dict.c` 是由公开数据表生成的纯数据文件。

---

## ⭐ About This Project

这个项目的重点并不是单纯实现一个电子书阅读 UI，而是尝试在资源有限的经典 ESP32 上完成：

```text
GUI
+
WiFi
+
HTTP / HTTPS
+
HTML Parsing
+
gzip
+
Chinese Input
+
Encoding Conversion
+
Flash Cache
+
TF Storage
+
Persistent Reading State
```

从而构建一个可以独立运行的嵌入式阅读设备。

更多文档：

| 文档 | 内容 |
| --- | --- |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | 环境搭建、构建烧录、主机测试、字体重建、新增书源 |
| [`HANDOFF.md`](HANDOFF.md) | 实现现状、逐次真机验收记录、文档与代码偏差清单 |
