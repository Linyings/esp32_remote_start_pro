# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Set the components to include explicitly
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

# Include the ESP-IDF project.cmake file
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Define the project
project(ESP32_BUTLER_WEB)

# After project() call, we can use ESP-IDF functions
# Register the partition for web_data
spiffs_create_partition_image(web_data ${CMAKE_CURRENT_SOURCE_DIR}/web_content FLASH_IN_PROJECT)
