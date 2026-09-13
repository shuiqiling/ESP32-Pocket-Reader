# legacy/

已废弃的早期实现，**不参与编译**，保留在此仅作技术记录。

## `wifi_loader.c` / `wifi_loader.h`

项目最初的原型：ESP32 自己开一个名为 `ESP32-Reader` 的 WiFi AP，电脑连上这个热点、在 `http://192.168.4.2:8000` 起一个本地服务器，由 **电脑负责抓取小说**，ESP32 只做下载端。

```text
PC (crawler + HTTP server 192.168.4.2:8000)
        ▲
        │  ESP32 连入 AP
        │
ESP32 (AP + 下载端)  ──►  SPIFFS
```

被废弃的原因很直接：**用户明确要求抓取与解析必须由 ESP32 自己完成，不接受电脑中转**。当前实现改为 ESP32 直接以 STA 模式连接路由器、自行请求网页并解析（见 `main/web_scraper.c` 与 `main/web_html.c`）。

对应当年电脑端的配套脚本是 `tools/serve_novel.py`，同样不再作为主推方案，且未纳入仓库。

## 为什么还留着

- 记录“AP 中转”这条被否掉的路线，避免后来者重复提案；
- `wifi_loader.c` 里的 AP 模式初始化、HTTP 客户端下载到 SPIFFS 的写法，在需要 ESP32 自建热点配网的场景下仍有参考价值。

## 如果要重新启用

1. 把 `.c` / `.h` 移回 `main/`；
2. 在 `main/CMakeLists.txt` 的 `SRCS` 中加入 `"wifi_loader.c"`；
3. 注意它与当前 `main/wifi_setup_ui.c` 的 STA 配网流程**互斥**——两者都会初始化 WiFi，同时启用需要先决定由谁调用 `esp_wifi_init`。
