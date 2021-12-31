# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# used in components/esp8266/CMakeLists.txt to select which partition this
#  .bin file will be loaded; sets entry address accordingly
set(PARTITION_NAME main)

project(esp-idf-homekit)