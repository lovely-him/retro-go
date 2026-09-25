set(COMPONENT_SRCDIRS ". drivers/audio drivers/input drivers/usb fonts libs/netplay libs/lodepng")
# drivers/usb 进包含路径是为了让 drivers/input/usb_hid.c 能 #include "usb_host_svc.h"。
# 注意 usb_host_svc.h 会带进 usb/usb_host.h(-> hal/gpio_types.h -> esp_bit_defs.h 的
# BIT0..BIT31 对象宏), 所以它绝不能被任何核心源码包含; 只有 drivers/ 下的 USB 相关
# .c 可以用。rg_input.c 走的是 drivers/input/usb_hid.h, 那个头文件只用纯 C 类型。
set(COMPONENT_ADD_INCLUDEDIRS ". drivers/usb libs/netplay libs/lodepng")
if((IDF_VERSION_MAJOR EQUAL 5) AND (IDF_VERSION_MINOR GREATER 2)) # esp-idf 5.3+
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client esp_driver_ledc driver")
elseif(IDF_VERSION_MAJOR EQUAL 5) # esp-idf 5.0 - 5.2
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client")
else() # esp-idf 4.1 - 4.4
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update esp_adc_cal json nvs_flash esp_wifi esp_http_client")
endif()

# MIPI-DSI/DPI 显示驱动所需的额外组件:
#   esp_lcd        esp_lcd_new_dsi_bus / new_panel_io_dbi / new_panel_dpi
#   esp_mm         esp_cache_msync, 帧缓冲在 PSRAM 需回刷给 DSI 桥
#   esp_driver_i2c 背光 I2C(本板背光无 GPIO, 由屏端 I2C 器件控制)
#   esp_hw_support esp_ldo_acquire_channel, DSI D-PHY 需 LDO_VO3 供电
#   usb            USB Host Library, drivers/input/usb_hid.c 的 boot 键盘支持
#   esp_driver_i2s i2s_std, drivers/audio/es8311.c 的 I2S 放音通路
# 用字符串拼接而非 list(APPEND): 上面 COMPONENT_REQUIRES 是空格分隔的字符串。
# 按 IDF 版本而非按 target 名判断 —— 避免把 fork 专有的 target 名写进共享构建
# 逻辑; 且 IDF 4.x 没有 esp_mm 与 esp_driver_i2c, 无条件追加会破坏 4.4 的 target。
# usb 组件在 4.x 上也存在但不追加: usb_hid.c 整个文件被 RG_GAMEPAD_USB_MAP 包着,
# 没定义该宏的 target 得到的是空翻译单元, 不需要这个头文件路径。es8311.c 同理被
# RG_AUDIO_USE_ES8311 包着。
if(IDF_VERSION_MAJOR GREATER_EQUAL 5)
    set(COMPONENT_REQUIRES "${COMPONENT_REQUIRES} esp_lcd esp_mm esp_driver_i2c esp_hw_support usb esp_driver_i2s")
endif()

register_component()

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)
set_source_files_properties(
    rg_audio.c rg_display.c rg_utils.c rg_surface.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS
    -O2
)

if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_PROJECT_VER)
    component_compile_options(-DRG_PROJECT_VER="${RG_PROJECT_VER}")
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

component_compile_options(-D${RG_BUILD_TARGET}=1)
component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
