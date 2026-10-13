idf_component_register(SRCS "main.cpp" "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES m5stack__m5unified espressif__usb_device_uac)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${uac_dir}/tusb_uac")
