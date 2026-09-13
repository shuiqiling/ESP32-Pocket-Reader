idf_component_register(SRCS "main.c" "novel_reader.c" "fonts/novel_font_16.c"
                            "wifi_setup_ui.c" "web_scraper.c" "web_html.c"
                            "chapter_cache.c" "book_select_ui.c"
                            "display_brightness.c" "bookshelf.c"
                            "pinyin_dict.c" "touch_bitbang.c" "sd_storage.c"
                            "main_menu_ui.c" "txt_download_ui.c"
                            "gbk_codec.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_http_client
                                esp-tls mbedtls esp_rom nvs_flash spiffs fatfs
                                sdmmc esp_driver_sdspi esp_driver_spi)
