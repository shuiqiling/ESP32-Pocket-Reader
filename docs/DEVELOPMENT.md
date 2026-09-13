# 开发文档

面向需要在本机复现构建、修改代码或调试设备的开发者。

> 项目概览、功能说明与硬件接线见根目录 [`README.md`](../README.md)；
> 当前实现状态、历史验收记录与文档/代码偏差清单见 [`HANDOFF.md`](../HANDOFF.md)。

---

## 1. 环境准备

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| ESP-IDF | v5.5.4（`>= 5.4` 可用） | 官方安装器或 git clone 均可 |
| Python | 3.8+ | ESP-IDF 自带虚拟环境，`tools/*.py` 也可直接用系统 Python |
| Node.js | 18+ | 仅在重建中文字体时需要 |
| 串口驱动 | CH340 | ESP32-2432S028R 板载 USB 转串口 |

安装 ESP-IDF 后，在**每个新终端**里先执行导出脚本：

```powershell
# Windows (PowerShell / CMD)
%IDF_PATH%\export.bat
```

```bash
# Linux / macOS
. $IDF_PATH/export.sh
```

确认可用：

```bash
idf.py --version
```

### 常见问题

**`export.bat` 报 “This .bat file is for Windows CMD.EXE shell only”**
说明它被从 Git Bash / MSYS 环境里调用了（`MSYSTEM` 已设置）。要么改用 PowerShell，要么先清掉 `MSYSTEM`：

```bash
MSYSTEM= cmd.exe //c "export.bat && idf.py build"
```

**`export.bat` 报 Python 虚拟环境不存在，且路径里的版本号和你装的对不上**
`export.bat` 按 PATH 里第一个 `python` 的版本去拼 venv 目录名，所以系统装了 Python 3.13 而 IDF 的 venv 是 3.11 时，它会去找 `idf5.5_py3.13_env` 并失败。把 IDF 自带的解释器放到 PATH 最前面即可：

```bat
set PATH=D:\esp\Espressif\tools\idf-python\3.11.2;%PATH%
call D:\esp\esp-idf\v5.5.4\esp-idf\export.bat
```

（把版本号和路径换成 `%IDF_TOOLS_PATH%\tools\idf-python\` 下实际存在的那个。）

---

## 2. 构建与烧录

```bash
idf.py set-target esp32     # 首次或切换目标时执行
idf.py build
idf.py -p <PORT> flash monitor
```

串口号：

- Windows：设备管理器 → 端口，形如 `COM8` / `COM12`。CH340 会随 USB 重枚举在两个号之间跳动，烧录前先确认。
- Linux：`/dev/ttyUSB0`，可能需要把当前用户加入 `dialout` 组。

### ⚠️ 烧录会覆盖 SPIFFS

`main/CMakeLists.txt` 中的 `spiffs_create_partition_image` 会在构建时生成 SPIFFS 镜像，完整 `idf.py flash` 会一并烧录，**覆盖设备上运行期的章节缓存与目录索引**。

只迭代应用代码时，单独烧录应用分区即可保留 NVS 与缓存：

```bash
idf.py -p <PORT> app-flash monitor
```

NVS 分区不会被 `app-flash` 或 `flash` 擦除，WiFi 凭据与阅读进度在正常迭代中都会保留。

### 分区表

`partitions.csv` 针对 4MB Flash：

```text
nvs       24 KB
phy        4 KB
factory    3 MB
spiffs   960 KB
```

应用分区已接近上限，新增较大模块前先看 `idf.py size` 输出。

---

## 3. PC 端单元测试

与硬件无关的模块可以直接用 gcc 在 PC 上跑，不需要开发板。**命令在仓库根目录执行。**

### 3.1 章节缓存窗口

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Imain \
    tools/chapter_cache_selftest.c main/chapter_cache.c \
    -o chapter_cache_selftest
./chapter_cache_selftest
```

预期输出 `CHAPTER CACHE SELFTEST PASSED`（非零退出即失败）。

### 3.2 HTML 解析

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic \
    tools/web_html_selftest.c main/web_html.c \
    -o web_html_selftest
./web_html_selftest
```

预期输出 `SELFTEST PASSED`。

测试输入来自 `tools/fixtures/` 下的**合成页面**，加上测试文件内的内联合成用例（实体解码、链接去重、`_2`/`_3` 分页顺序、循环保护、广告过滤等）。仓库不包含任何抓取到的第三方页面。

### 3.3 用真实页面调试解析器

书源结构变化时，先用浏览器把页面另存到本地，再喂给离线探针：

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic \
    tools/book_source_probe.c main/web_html.c -o book_source_probe

./book_source_probe search.html detail.html chapter.html
```

探针不联网，只对传入的 HTML 跑解析逻辑并打印结果，适合定位“搜索为空 / 正文缺失 / 下一章链接不对”这类问题。

> 如果 `main/web_html.c` 修改了解析规则，记得同步更新 `tools/web_html_selftest.c` 的断言，否则下次改代码时测试会失去意义。

---

## 4. 重建中文字体

字体为 1bpp、无压缩的 LVGL 自定义字体，字形存放在 Flash 中，不占用运行期大块 RAM。

```bash
npm install                 # 首次执行，安装 lv_font_conv@1.5.3
node tools/build_font.js
```

脚本会：

1. 收集 ASCII 可打印字符、GB2312 常用字、搜索模板用到的常用 Big5 字；
2. 扫描 `main/*.c` 的**字符串字面量**，把 UI 文案用字并入字符集（避免界面上出现豆腐块）；
3. 扫描 `spiffs_data/*.txt` 中的字符；
4. 调用 `lv_font_conv` 生成 `main/fonts/novel_font_16.c`，并把 fallback 补成 `lv_font_montserrat_16`。

### 源字体必须可再分发

生成的字体内嵌了字形轮廓，源字体必须是允许再分发的授权字体。脚本按以下顺序查找，也可用 `FONT_SOURCE` 环境变量指定：

```text
$FONT_SOURCE
C:/Windows/Fonts/Noto Sans SC Medium (TrueType).otf
C:/Windows/Fonts/Noto Sans SC (TrueType).otf
C:/Windows/Fonts/NotoSansSC-VF.ttf
/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc
```

当前使用的是 **Noto Sans SC**——即 Google Fonts 发布的**思源黑体**，SIL OFL 1.1 授权。选 Medium 字重是因为 16px 1bpp 光栅化会吃掉细体笔画。

**不要用 `simhei.ttf`（中易黑体）、`msyh.ttc`（微软雅黑）等 Windows 随附商业字体**：其许可禁止再分发衍生字形，生成的 `.c` 文件不能公开。

Linux 上若只有 `.ttc` 集合，`lv_font_conv` 默认取第一个 face，必要时先用 fonttools 拆出单个 `.otf`：

```bash
pip install fonttools
fonttools ttLib.woff2 decompress ...   # 或 python -m fontTools.ttx
```

---

## 5. 新增 / 修改书源

在线阅读的解析规则集中在两个文件：

| 文件 | 职责 |
| --- | --- |
| `main/web_scraper.c` | 站点地址、HTTP 请求、gzip 解压、镜像切换、写盘 |
| `main/web_html.c` | 搜索结果、书籍详情、标题、正文、下一章链接的解析 |

改书源时的一般流程：

1. 在 `web_scraper.c` 顶部的 `BASE_SCHEME_HOST` / `SEARCH_SCHEME_HOST` 附近确认地址；
2. 浏览器另存搜索页、详情页、章节页；
3. 用 `book_source_probe` 验证解析结果；
4. 把**匿名化后的最小结构**补进 `tools/fixtures/`，并在 `tools/web_html_selftest.c` 里加断言；
5. 真机验证搜索 → 详情 → 首章 → 下一章全链路。

镜像故障转移只对搜索/详情请求生效（`can_failover` 判断 URL 前缀），正文与目录请求固定主站、只做同主机重试。新增镜像时要同步这个判断。

---

## 6. 调试建议

### 串口日志

```bash
idf.py -p <PORT> monitor
```

`monitor` 下 `Ctrl+]` 退出。设备异常重启时先看 backtrace，再用 `idf.py monitor` 自带的解码，或：

```bash
xtensa-esp32-elf-addr2line -pfiaC -e build/ESP32-Pocket-Reader.elf <地址...>
```

### 内存

在线抓取期间容易出现的问题是大块**连续**堆不足，而不是总堆不足。观察日志里的 `largest free block`；章节正文按 `292 × 156` 像素视口分页，单页缓冲和解析缓冲是主要占用方。

### 触摸

触摸为 GPIO 时序驱动的 XPT2046，映射参数在 `main/main.c` 的 `xpt2046_cfg` 附近（`x_max` / `y_max` / `swap_xy` / `mirror_x` / `mirror_y`）。出现左右或上下颠倒时先调这四个开关，再考虑改量程。

### SPIFFS 缓存

运行期缓存目录为 `/spiffs`，文件名为 `cache_%03d.txt`；本地数字章节（`001.txt` 等）与它互不干扰。缓存异常时可以直接擦除 SPIFFS 分区重来：

```bash
idf.py -p <PORT> erase-flash     # 会清掉 NVS，WiFi 需要重新输入
```

---

## 7. 提交前检查

```bash
node tools/build_font.js      # 改了 UI 文案或 spiffs_data 内容时
./chapter_cache_selftest
./web_html_selftest
idf.py build
```

- 不要提交 `build/`、`managed_components/`、`node_modules/`；
- 不要提交抓取到的网页快照与小说正文（`.gitignore` 已覆盖常见文件名，新增样本请放 `tools/fixtures/` 并自行匿名化）；
- 不要提交参赛/申报材料；
- 功能或配置有改动时，同步更新 `README.md`、`HANDOFF.md`，并把新的文档/代码偏差补进 `HANDOFF.md` 第 8 节。
