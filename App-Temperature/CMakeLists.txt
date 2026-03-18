cmake_minimum_required(VERSION 3.14)

add_compile_definitions(APP_NAME="${APP_1_NAME}")
add_compile_definitions(APP_VERSION="${APP_1_VERSION_NUMBER}")
add_compile_definitions(BUILD_NUM=${BUILD_NUMBER})

add_executable(${APP_1_NAME}
    ${APP_1_SRC_DIRECTORY}/main.c
)

target_link_libraries(${APP_1_NAME} LINK_PUBLIC
    pico_stdlib
    hardware_adc
    hardware_pio
    FreeRTOS
)

pico_enable_stdio_usb(${APP_1_NAME} 1)
pico_enable_stdio_uart(${APP_1_NAME} 1)

pico_add_extra_outputs(${APP_1_NAME})