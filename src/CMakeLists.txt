idf_component_register(SRCS "main.cpp" "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES m5stack__m5unified espressif__usb_device_uac espressif__esp-dsp)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${uac_dir}/tusb_uac")

# M5Unified 0.2.17's unused PowerHub RTC driver warns under -O2. Keep the
# warning visible without weakening diagnostics for our firmware or ESP-IDF.
# PlatformIO imports per-component compile options from ESP-IDF's CMake model.
idf_component_get_property(m5_lib m5stack__m5unified COMPONENT_LIB)
target_compile_options(${m5_lib} PRIVATE -Wno-error=maybe-uninitialized)
