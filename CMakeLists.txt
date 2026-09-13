cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_novel_reader)

# Create SPIFFS image from the local spiffs_data directory at build time.
# The image is flashed together with the app using `idf.py flash`.
spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
