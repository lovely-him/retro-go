# This file is injected late into rg_tool.py, you can run arbitrary python code here
# For example override python variables or set environment variables with os.putenv

# Espressif chip in the device
IDF_TARGET = "esp32p4"
# .fw file format, if supported by the device
# none -> rg_tool.py 走 build_image() 产出 .img，使用 mkfw.py 内置的 esp32p4 偏移
# (bootloader 0x2000 / 分区表 0x8000 / 程序 0x10000)
FW_FORMAT = "none"
# Default apps to build when none is specified (comment to build all)
# DEFAULT_APPS = "launcher retro-core"

# networking 现在【不再强制关闭】: 这里刻意不给 args.no_networking 赋值, 让 rg_tool.py
# 的 --no-networking 命令行开关继续可用(默认 False -> 传 -DRG_ENABLE_NETWORKING=1)。
#
# 曾经写过 args.no_networking = True, 原因是 P4 没有射频: CONFIG_SOC_WIFI_SUPPORTED=0
# 使得 CONFIG_ESP_WIFI_* 那组 Kconfig 根本不生成, 而 IDF 自带的 esp_wifi.h 里
# WIFI_INIT_CONFIG_DEFAULT() 仍然引用它们, rg_network.c:290 一编译就报
#   error: 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM' undeclared
#
# 现在由 launcher/main/idf_component.yml 引入的 espressif/esp_wifi_remote 解决: 它在
# NOT CONFIG_ESP_WIFI_ENABLED 时把自己 include/injected/esp_wifi.h 前插到 esp_wifi 组件
# 的 INTERFACE_INCLUDE_DIRECTORIES, 那份头里的同一个宏改用 CONFIG_WIFI_RMT_*(由组件
# 自己的 Kconfig 生成, 与 SoC 有没有射频无关)。细节见该 yml 与本 target 的 sdkconfig。
#
# 不需要在这里按 app 区分: 四个核心各自在 CMakeLists 第 3 行硬写
# set(RG_ENABLE_NETWORKING 0), 会覆盖 rg_tool.py 传进来的值 —— 而
# components/retro-go/CMakeLists.txt:49-53 直接读这个变量, 说明覆盖确实能传到组件
# 作用域。所以实际只有 launcher 会带上网络栈。
# (args 在本文件被 exec() 时已是 rg_tool.py 的模块级全局, 需要时可直接改。)

# 覆盖分区尺寸。rg_tool.py 在模块级 exec() 本文件，故此赋值对后续 build_image() 生效，
# 且仅作用于本 target，不影响其它板卡。
#
# 上游默认值是按 4~8MB flash 的掌机定的，P4 的 RISC-V 代码普遍更大，按上游值有 4 个
# app 会超出声明尺寸，导致 mkfw.py 被动扩容并告警。此处按实测体积分配，使布局成为
# 有意设计，同时保证 flash_app() 单 app 增量刷写在 app 变大后仍可用。
#
# 余量刻意留到 20% 以上：显示驱动、USB HID 输入、ES8311 音频、GT9xx 触摸都位于共享的
# retro-go 组件里，加进去会让【每个】app 一起变大。实测每次的增量：MIPI-DSI 显示约
# +50KB，USB Host 协议栈约 +75KB，esp_driver_i2s + ES8311 约 +29KB，GT9xx 触摸
# (芯片驱动 + 虚拟按键层 + rg_display 覆盖层钩子) 约 +4KB。
#
# 唯一的例外是 launcher 的 Wi-Fi：esp_hosted + esp_wifi_remote + eppp + lwIP + mbedtls
# + esp_http_server + 内嵌网页一次性吃掉 **+634KB**（701KB -> 1335KB），所以它的槽位
# 从 0x0E0000 抬到了 0x1C0000。这部分只落在 launcher，四个核心不受影响。
# 若日后要给 launcher 瘦身，sdkconfig 里 CONFIG_ESP_HOSTED_CLI_ENABLED 与
# CONFIG_WIFI_RMT_ENTERPRISE_SUPPORT 是最明显的两块（控制台 CLI 与 WPA2-Enterprise，
# 都用不上），但它们属于 esp-claw 已验证配置的一部分，改动前请单独验证。
#
# 实测基准: 1.46-8-g4ced1 + MIPI-DSI 显示 + USB HID 键盘 + ES8311 音频 + GT9xx 触摸
#                        + launcher Wi-Fi(ESP-Hosted 2.12.3)
#                        Type, SubType, Size       实测     -> 声明      占用
PROJECT_APPS = {
  'launcher':   [0, 16, 0x1C0000],  #  0x14de30 = 1335 KB -> 1792 KB    75%
  'retro-core': [0, 17, 0x1C0000],  #  0x1454e0 = 1301 KB -> 1792 KB    73%
  'prboom-go':  [0, 18, 0x160000],  #  0x119140 = 1124 KB -> 1408 KB    80%
  'gwenesis':   [0, 19, 0x1A0000],  #  0x142910 = 1290 KB -> 1664 KB    78%
  'fmsx':       [0, 20, 0x120000],  #  0x0e7450 =  925 KB -> 1152 KB    80%
}
# 合计 0x7A0000 = 7.63MB，加镜像头 0x10000 约 7.69MB。板载 16MB flash，余量仍充足
# (fmsx 与 prboom-go 已到 80%，下一次往共享组件里加东西时要顺手扩容)。
# 分区尺寸变更后必须整片烧录(build-img + install)，不能用 flash/run 单 app 增量刷写：
# 那两个命令只写 app 分区、不动分区表，设备上留着旧表会导致切 app 时偏移对不上。
