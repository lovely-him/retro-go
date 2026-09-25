// Target definition
#define RG_TARGET_NAME             "ESP32-P4-WIFI6-DEVKIT"

// 硬件事实的唯一定义源: esp32-p4-platform/board/esp32p4_wifi6_dev_kit.h
// (Waveshare ESP32-P4-WIFI6-DEV-KIT, 丝印 Rev1.2, 芯片 P4 rev v3.1+)
//
// 按 retro-go 约定本文件不含任何 #include: GPIO_NUM_x / SDMMC_* / SDMMC_FREQ_*
// 等都是裸 token, 由真正使用它们的 .c 文件负责解析。
// 不要在此 include 板级头文件 —— 它会经 driver/gpio.h 引入 esp_bit_defs.h 的
// BIT0..BIT31 对象宏, 与 snes9x 的 BIT8()/BIT16() 函数(65C816 BIT 指令模拟)冲突,
// 导致 retro-core 编译失败。gnuboy/nofrendo/smsplus/gwenesis 也各自定义了 BIT()。

// I2C 总线(板载, 共享)
// 同一条 I2C1 上挂着三个器件: 屏端转接板的电源/背光控制器 0x45、音频 codec ES8311
// 0x18、电容触摸 GT911 0x5D/0x14。
//
// 总线由 rg_i2c.c 统一持有, 任何驱动都不得自己 i2c_new_master_bus() —— 同一端口建
// 第二条总线会失败。各驱动在自己 init 里调 rg_i2c_init()(它幂等, 先到的建总线),
// 然后用 rg_i2c_read/write_byte() 访问; 需要非默认速率的先调 rg_i2c_set_speed()。
//
// RG_I2C_MASTER_DRIVER 让 rg_i2c.c 走 IDF 新版 i2c_master API 而不是 legacy
// driver/i2c.h。必须走新版: 新 API 的 SCL 速率是【每设备】设置的, 而本板背光控制器
// 实测只能 100kHz、codec 可以 400kHz, legacy 只有总线级速率做不到共存。
// 引脚宏沿用上游命名 RG_GPIO_I2C_SDA/SCL(不是 RG_DISP_*, 因为它不属于显示模块)。
//
// ── 本文件里"开关宏"的约定 ────────────────────────────────────────────────────
// 能力开不开, 用【定义 / 不定义】表示, 宏不带数值, 门控一律 #ifdef。只有当一个能力
// 内部还要分型号或分支时, 才用数值 0/1/2 加 #if 选择(上游的 RG_SCREEN_DRIVER、
// RG_BATTERY_DRIVER、RG_I2C_GPIO_DRIVER 都是这种)。
// 最糟的组合是 `#define X 1` 配 #ifdef 门控: 把 1 改成 0 并不能关掉它, 会静默失效。
// 注意区分开关宏与【参数宏】—— 下面 RG_I2C_INTERNAL_PULLUP、RG_I2C_PORT、各 I2C
// 地址与速率都是要传给代码的值, 不是开关, 照旧写数值。
// 上游自己拥有的 RG_AUDIO_USE_INT_DAC / EXT_DAC 仍保持 0/1: rg_audio.c 用 #if 消费
// 它们, 那是上游的形状, 不改。
#define RG_I2C_MASTER_DRIVER
#define RG_I2C_PORT                 1
#define RG_GPIO_I2C_SDA             GPIO_NUM_7
#define RG_GPIO_I2C_SCL             GPIO_NUM_8
#define RG_I2C_INTERNAL_PULLUP      0           // 板载外部上拉, 见板级头文件第 1 节
// 未登记速率的地址用这个默认值。取 100kHz 而不是板级头文件声称的 400kHz: 厂商 BSP
// 对 0x45 显式写 .scl_speed_hz = 100*1000(esp32_p4_platform.c:460/660/740), 对
// ES8311 则经 esp_codec_dev 的 DEFAULT_I2C_CLOCK = 100000。也就是说该头文件的 400kHz
// 声明在两个已知器件上都被厂商自己的代码推翻了。唯一例外是 GT911 触摸, BSP 用的是
// CONFIG_BSP_I2C_CLK_SPEED_HZ, 将来接触摸时可以单独登记更高速率。
#define RG_I2C_SPEED                100000

// Storage
// 板载 TF 卡为 SDMMC slot0, 硬件支持 4-bit, 但 SD VDD 由片内 LDO_VO4@3300mV 供给,
// 必须先建供电控制句柄交给 host, 否则卡完全没电, ACMD41 协商直接超时(0x107)。
// rg_storage.c 的 SDMMC 分支据 RG_STORAGE_SD_PWR_LDO_CHAN 走片内 LDO 供电路径。
// GPIO45 上的 P-MOS 是 VDD 通断控制(低有效), 但栅极有外部下拉、不驱动时硬件默认
// 导通, 板卡仓库的 09_sdmmc 例程也只是把它配成输入读电平验证, 因此无需驱动。
// 暂仍按 1-bit 接线: rg_storage.c 硬编码 SDMMC_HOST_FLAG_1BIT 且只设 clk/cmd/d0。
// 1-bit @20MHz 约 2.5MB/s, 读 ROM 足够; 4-bit 待确认 1-bit 能挂载后再单独加。
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_0
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
#define RG_STORAGE_SD_PWR_LDO_CHAN  4
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_43
#define RG_GPIO_SDSPI_CMD           GPIO_NUM_44
#define RG_GPIO_SDSPI_D0            GPIO_NUM_39

// Audio
// 板载 ES8311 codec + 功放 + 麦克风 + 3.5mm 耳机口。驱动见 drivers/audio/es8311.c。
//
// INT_DAC / EXT_DAC 都保持 0: P4 没有片内 DAC; 而 drivers/audio/i2s.c 服务的是
// "无需配置的哑 DAC", 既引用了 IDF 5.5 已移除的 <driver/i2s.h>, 又把 mck_io_num
// 硬设为 GPIO_NUM_NC —— ES8311 是带内部时钟树的 codec, 必须要 MCLK。两者都置 0
// 可让该文件被条件编译整体剔除, 不影响构建。
//
// ES8311 工作在 I2S 从机模式, 时钟全部由 P4 提供: MCLK = 256 x fs, BCLK = MCLK/8
// = 32fs, 正好是 16-bit 立体声每帧所需。MCLK 恒为 256 倍这个约束是整个音频设计的
// 基石(它让 codec 的分频配置与采样率无关), 推导见 es8311.c 文件头。
// 只接放音通路: 麦克风(GPIO11)不用, 所以 I2S 只建 TX 通道, 省下 RX 那份 DMA 缓冲。
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        0
// 上面两个是上游宏(rg_audio.c 用 #if 消费, 且 INT_DAC 的取值本身就是 0/1/2/3 的分支
// 选择器), 形状不动。ES8311 是本 fork 新增的能力、内部没有分支要选, 所以按本文件的
// 开关约定用"定义 / 不定义"表示, 门控是 #ifdef。
#define RG_AUDIO_USE_ES8311

#define RG_AUDIO_I2S_PORT           I2S_NUM_1
#define RG_GPIO_SND_I2S_MCLK        GPIO_NUM_13
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_12
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_10     // LRCK
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_9      // P4 -> codec, 放音

// 功放使能, 【高有效】。刻意不定义 RG_GPIO_SND_AMP_ENABLE_INVERT —— 按 i2s.c 的
// 约定, 那个宏表示"静音 = 高电平"。
#define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_53

// ES8311 挂在共享的 I2C1 上(总线定义见上面的 "I2C 总线(板载, 共享)" 一节)。
// 7-bit 地址 0x18(厂商代码里的 ES8311_CODEC_DEFAULT_ADDR = 0x30 是 8-bit 形式,
// esp_codec_dev 建器件时会 >>1)。速率取 100kHz —— 厂商 esp_codec_dev 的
// DEFAULT_I2C_CLOCK 就是这个值, 板级头文件声称的 400kHz 未经验证。
#define RG_AUDIO_CODEC_I2C_ADDR     0x18
#define RG_AUDIO_CODEC_I2C_SPEED    100000

// Video
// 板载 MIPI-DSI 2-lane + JD9365 面板(Waveshare 10.1-DSI-TOUCH-A), 物理 800x1280 RGB565。
// 面板参数与 200 条 DCS 初始化序列见 drivers/display/jd9365_waveshare_10p1.h。
//
// RG_SCREEN_WIDTH/HEIGHT 是【逻辑分辨率】, 不是面板物理分辨率; 放大与定位由驱动完成。
// 取 320x240 = retro-go 全部 17 个 target 的设计基准尺寸, 因此 rg_gui.c 的布局零风险。
//
// 缩放规则: 倍数只从 {0.5, 1, 2} 里取, 按面板最短边自适应选最大可行值 ——
//   240 * 2 = 480 <= 800(面板最短边) ✓, 而 3 倍不在允许集合内, 故 scale = 2。
// 只允许整数(或半整数)倍是为了点阵字体: 2.5x 最近邻会让 1px 笔画时而 2px 时而 3px,
// 同一行文字粗细不均, 主观上就是"糊"(实测过, 辨识度明显差于 2x)。
//
// 定位规则: 内容 640x480, 水平居中(左右各 80px 黑), 垂直顶部对齐(下方 800 行黑)。
// 驱动在 init 时把整个帧缓冲清黑, 因为 rg_display 只写脏区域、从不碰边框。
// 下方黑区【不再】是留给虚拟按键的: 按键改为叠加在正式画面上(见 Touch 段), 这样
// 竖屏横屏可以用同一套布局, 且按键区随实际画面自适应。竖屏下这块黑区暂时空着。
//
// 注: LCD_BUFFER_LENGTH = RG_SCREEN_WIDTH * 4 = 1280 像素 = 2.5KB 条带缓冲。
#define RG_SCREEN_DRIVER            2
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}

// 整数放大倍数。驱动据此算出面板内偏移(水平居中、垂直顶部对齐), 见 mipi_dsi.h。
#define RG_DSI_SCALE_X              2
#define RG_DSI_SCALE_Y              2

// 内容在面板内的偏移(结果值)。驱动本来自己算, 但触摸命中判定需要把面板坐标换算
// 回逻辑坐标, 而触摸模块看不到 mipi_dsi.h(它只被 rg_display.c 按 RG_SCREEN_DRIVER
// 选中), 所以把结果提到 config.h 作为唯一来源。
// mipi_dsi.h 用 #if 校验这两个值与"(面板宽 - 内容宽)/2"和"垂直顶部对齐"一致, 不符
// 就编译失败 —— 因此不存在两处漂移的可能。
//   X = (800 - 320*2) / 2 = 80     Y = 0
#define RG_DSI_OFFSET_X             80
#define RG_DSI_OFFSET_Y             0

// MIPI DSI D-PHY 供电: 片内 LDO_VO3 @ 2500mV, 必须在建 DSI 总线之前使能,
// 否则 PHY 无法从 No Power 进入 Shutdown 状态。
#define RG_DSI_PHY_LDO_CHAN         3
#define RG_DSI_PHY_LDO_MV           2500

// 屏端转接板上的电源/背光控制器(I2C 从机)。它不只是背光:
//   寄存器 0x95 = 面板上电使能, 必须先写 0x11 再写 0x17
//   寄存器 0x96 = 背光亮度, 0x00~0xFF
// 上电时序不对, 面板就不会响应 DCS; 而 IDF 把 DSI 主机的硬件超时显式禁用
// (esp_lcd_mipi_dsi_bus.c 里 set_timeout_count 全传 0), 一次读命令就会永久自旋。
// 总线的端口/引脚/上拉见上面的 "I2C 总线(板载, 共享)" 一节。
#define RG_DISP_I2C_ADDR            0x45
#define RG_DISP_REG_POWER           0x95
#define RG_DISP_REG_BACKLIGHT       0x96
// 板级头文件声称板载器件均为 400kHz, 但厂商面板驱动实测用 100kHz。以能跑通的为准。
// 这是【本设备】的速率, 不影响同总线上 GT911 跑 400kHz —— 新 API 按设备设速率。
#define RG_DISP_I2C_SPEED           100000

// Input
// 板卡无游戏按键。BOOT 键在 GPIO35, 与以太网 RMII TXD1 复用 —— 已对照原理图与数据
// 手册确认这是硬件设计如此, BOOT 仅在上电时用到, 不影响运行期功能, 故不作为运行期
// 输入候选。用户按键与状态灯均为 NC。
// 因此输入只能来自外设: USB HID 键盘(已实现, 见下) 与 GT911 屏幕虚拟按键(驱动已
// 实现, 见再下面的 Touch 段; 按键映射与渲染待做)。
//
// 板载 USB 为 OTG High-Speed + Type-A 口, 经 CH334 hub, 靠跳线帽切到 HOST 侧。
//
// USB 分两层: drivers/usb/usb_host_svc.c 是与设备类型无关的主机服务(安装 USB Host
// Library、注册客户端、跑事件泵、发现设备、open/close、分发拔出事件), 由
// RG_USB_HOST_ENABLE 开启; 具体的类驱动通过 probe/poll/detach 三个回调注册进去。
// drivers/input/usb_hid.c 是第一个类驱动, 只支持 boot protocol 键盘 —— 报告格式被
// HID 1.11 固定为 8 字节, 因而无需解析报告描述符。将来接手柄或 U 盘时新增一个类
// 驱动即可, 不必再碰主机生命周期那部分。
#define RG_USB_HOST_ENABLE

// 下面的 HID_KEY_* / HID_MOD_* 与本文件其它裸 token 同理: config.h 不含 #include,
// 宏在 rg_input.c 实例化 keymap_usb[] 处展开, 该处已先包含 drivers/input/usb_hid.h。
// 刻意不定义 RG_GAMEPAD_ADC_MAP: rg_input.c 的 ADC 分支调用 adc1_config_width() 等
// IDF 5.5 已移除的接口。
// 键位取值: 方向键与 A/B/START/SELECT/MENU 是本 target 早先定下、已经用熟的一套, 不
// 动; 新增的 X/Y/L/R/OPTION 直接采用上游 targets/sdl2/config.h 里 RG_GAMEPAD_KBD_MAP
// 的取值(X=S, Y=A, L=Q, R=W, OPTION=Tab), 不自己凭空发明。注意"X 键发出 RG_KEY_B、
// S 键发出 RG_KEY_X"这种错位是上游本来就有的形状, 不是笔误。
//
// X/Y/L/R 目前【没有任何核心读取】(全仓库只有 rg_input.c 的按键名字表和 sdl2 /
// t-deck-plus 两个 target 引用它们)。多键主机靠核心自带的重映射表解决, 例如
// main_snes.c 的 Type A/B/C: SNES 的 X<-START、Y<-SELECT、L<-B+MENU、R<-A+MENU。
// 这里仍然把它们映射全, 是为了让输入层成为一个完整的 14 键手柄, 将来接新核心或自己
// 写东西时不必再回头改。
#define RG_GAMEPAD_USB_MAP             \
    {                                  \
        {RG_KEY_UP, 0, HID_KEY_ARROW_UP},           \
        {RG_KEY_DOWN, 0, HID_KEY_ARROW_DOWN},       \
        {RG_KEY_LEFT, 0, HID_KEY_ARROW_LEFT},       \
        {RG_KEY_RIGHT, 0, HID_KEY_ARROW_RIGHT},     \
        {RG_KEY_A, 0, HID_KEY_Z},                   \
        {RG_KEY_B, 0, HID_KEY_X},                   \
        {RG_KEY_X, 0, HID_KEY_S},                   \
        {RG_KEY_Y, 0, HID_KEY_A},                   \
        {RG_KEY_L, 0, HID_KEY_Q},                   \
        {RG_KEY_R, 0, HID_KEY_W},                   \
        {RG_KEY_START, 0, HID_KEY_ENTER},           \
        {RG_KEY_SELECT, HID_MOD_RSHIFT, 0},         \
        {RG_KEY_MENU, 0, HID_KEY_ESC},              \
        {RG_KEY_OPTION, 0, HID_KEY_TAB},            \
    }

// Touch
// Goodix GT9xx 电容触摸, 与背光 0x45、ES8311 0x18 共用 I2C1(SDA=GPIO7 / SCL=GPIO8),
// 只是地址不同 —— 板上只有一条 I2C 总线, 屏端背光和触摸都挂在上面。
//
// 芯片实测自报 "927"(产品 ID 39 32 37, 配置版本 0x82), 是 GT927 而非 GT911; 厂商
// BSP 对这块板用的却是 esp_lcd_touch_gt911。GT9xx 家族共用同一套寄存器映射, 四角
// 实测也已逐条验证, 所以驱动名沿用 gt911。
//
// 0x5D 是主地址, 0x14 是上电复位瞬间 INT 拉高才会选中的备用地址。本板 INT 是 NC,
// 所以由驱动探测决定; rg_i2c_probe() 只做一次地址握手、不 add_device, 探不到的那个
// 不会在从机表里留下死槽位。**实测命中主地址 0x5D**, 与厂商 board_check.c 记录的
// 总线扫描基线 "0x18(ES8311) 0x45(屏端背光) 0x5D(GT911)" 一致。
//
// 400kHz 是厂商 BSP 单独给触摸设的(bsp_touch_new(): scl_speed_hz =
// CONFIG_BSP_I2C_CLK_SPEED_HZ = 400000), 与背光/codec 的 100kHz 不同。同一条总线上
// 两种速率并存是合法的: i2c_master.c:702 在每次事务开始时按该设备的速率重设总线时序。
//
// RST 与 INT 均未接线, 驱动因此只能轮询, 且绝不能写 0x8040 让芯片睡眠(唤醒要靠拉
// INT 脚, 本板做不到)。详见 drivers/input/gt911.c 顶部说明。
#define RG_TOUCH_DRIVER_GT911
#define RG_TOUCH_I2C_ADDR           0x5D
#define RG_TOUCH_I2C_ADDR_ALT       0x14
#define RG_TOUCH_I2C_SPEED          400000
// 屏的物理分辨率。已与芯片配置里的输出分辨率(0x8048..0x804B, 读出 800x1280)和四角
// 实测量程三方对齐。触摸坐标系: 原点左上, X 向右 0..800, Y 向下 0..1280, 不交换不
// 镜像; 多点实测至少 4 指同时上报, 且触点数组下标不等于 track id。具体实测数据记在
// drivers/input/gt911.h 的 rg_gt911_read() 注释里, panel -> logical 换算以那里为准。
#define RG_TOUCH_PANEL_W            800
#define RG_TOUCH_PANEL_H            1280

// 虚拟按键布局。坐标是【viewport 的千分比】(0..1000), 不是逻辑像素也不是面板像素:
// viewport 是 retro-go 算好的"真实画面"矩形(rg_display_get_info()->viewport), 它随
// 缩放模式和各核心的源宽高比自动变化, 所以用千分比描述就能让同一套布局在竖屏、将来
// 的横屏、以及 GB 这类非 4:3 源上自适应排布, 不需要为每种情况各写一份坐标。
// 用千分比而不是百分比, 是因为百分比在 320x240 的 viewport 上粒度只有 3.2px, 排小
// 按钮时误差可见; 千分比是 0.32px。整数是为了不在 config.h 里引入浮点字面量。
//
// 字段顺序: {按键, x, y, w, h}。x/w 是 viewport 宽度的千分比, y/h 是高度的千分比。
// 下面每行注释是当前 320x240 viewport 下按整数除法算出的逻辑像素(left, top, w, h),
// 改数值时对着它核对。14 个按钮互不重叠, 且全部落在 0..320 x 0..240 之内。
//
// 布局取向(照手柄来):
//   顶部一条  L | MENU | OPTION | R          —— 肩键在两个上角, 两个菜单键居中
//   左下      十字键(3x3 网格取加号位)
//   右下      面键菱形, SNES 方位: X 上 / Y 左 / A 右 / B 下
//   底部中央  SELECT | START
// 中间大片区域刻意留空, 那是游戏画面最常被看的部分。
#define RG_GAMEPAD_TOUCH_MAP                                            \
    {                                                                   \
        /* 顶部肩键与菜单键, 扁条 */                                     \
        {RG_KEY_L,        8,   6, 110, 55},  /* (  2,  1) 35x13 */      \
        {RG_KEY_MENU,   330,   6, 140, 55},  /* (105,  1) 44x13 */      \
        {RG_KEY_OPTION, 530,   6, 140, 55},  /* (169,  1) 44x13 */      \
        {RG_KEY_R,      882,   6, 110, 55},  /* (282,  1) 35x13 */      \
        /* 十字键: 单元格 100x140 千分比 = 32x33 逻辑像素 */              \
        {RG_KEY_UP,     115, 545, 100, 140},  /* ( 36,130) 32x33 */     \
        {RG_KEY_LEFT,    15, 690, 100, 140},  /* (  4,165) 32x33 */     \
        {RG_KEY_RIGHT,  215, 690, 100, 140},  /* ( 68,165) 32x33 */     \
        {RG_KEY_DOWN,   115, 835, 100, 140},  /* ( 36,200) 32x33 */     \
        /* 面键菱形: 单元格 100x130 千分比 = 32x31 逻辑像素 */            \
        {RG_KEY_X,      785, 590, 100, 130},  /* (251,141) 32x31 */     \
        {RG_KEY_Y,      680, 725, 100, 130},  /* (217,174) 32x31 */     \
        {RG_KEY_A,      890, 725, 100, 130},  /* (284,174) 32x31 */     \
        {RG_KEY_B,      785, 860, 100, 130},  /* (251,206) 32x31 */     \
        /* 底部中央 */                                                   \
        {RG_KEY_SELECT, 370, 930, 110,  62},  /* (118,223) 35x14 */     \
        {RG_KEY_START,  520, 930, 110,  62},  /* (166,223) 35x14 */     \
    }

// 多久没有触摸就隐藏虚拟按键。隐藏就是停止合成: 覆盖层没有能力擦除自己画过的像素,
// 但应用下一次重画该区域时会自然盖掉它 —— 游戏里是下一帧(约 33ms), launcher 里是
// 下一次交互。所以不需要任何擦除机制。代价是画面完全静止又没有输入时(无动画的标题
// 画面)按钮会留着, 直到内容变化; 那是观感问题, 不是功能问题。
#define RG_TOUCH_HIDE_TIMEOUT_S     5

// Battery
// 开发板无电池。置 0 可同时避开 esp_adc_cal.h 与 legacy ADC 表征调用。
#define RG_BATTERY_DRIVER           0

// Networking
// P4 没有射频, Wi-Fi 经 4-bit SDIO 走板载 ESP32-C6 协处理器(ESP-Hosted 2.12.3)。
// 组件依赖在 launcher/main/idf_component.yml, 引脚与缓冲配置在本 target 的 sdkconfig。
// 只有 launcher 带网络栈: 四个核心各自在 CMakeLists 里硬写 RG_ENABLE_NETWORKING 0。
//
// 打开 networking 后 launcher 会多出三个上游现成的功能, 都不需要新代码:
//   1. "File server" 菜单项(main.c:424 -> webui.c): 内嵌 HTTP 服务器 + 网页,
//      GET / 是界面, GET /* 下载, PUT /* 上传, POST /api 是 JSON 接口。浏览器打开
//      设备 IP 就能往 SD 卡拖 ROM。
//   2. "Check for updates" 菜单项(main.c:436 -> updater.c): 抓下面这个 releases JSON。
//   3. NTP 校时: rg_network.c:294-296 已经配好 pool.ntp.org, :80-81 在拿到 IP 的事件里
//      自动 esp_sntp_init()。校时成功后 rg_system_save_time() 会写回
//      /sd/retro-go/cache/clock.bin, 于是 rg_system_load_time() 下次开机就是对的 ——
//      本板没有 RTC 电池, 在此之前时钟一直是从构建时间推算的, 是错的。
//
// netplay 不在此列: libs/netplay/rg_netplay.c 整个被 #ifdef RG_ENABLE_NETPLAY 包着,
// 而这个宏在全仓库从来没有被 set 过(只有 base.cmake:22 与 components/retro-go/
// CMakeLists.txt:49 在读它), 所以上游默认就是关闭状态。而且它的入口 rg_gui.c:2285
// 在游戏内菜单里、跑在核心中, 要启用就得让四个核心都背上 Wi-Fi 栈 —— 核心里内部 RAM
// 只剩约 65KB 空闲 / 最大连续块 31KB, lwIP 与 SDIO DMA 缓冲要的正是内部 RAM, 装不下。

// 更新检查的数据源。上游 components/retro-go/config.h:98 用 #ifndef 包着这个宏, 所以
// target 可以直接覆盖。默认值指向 ducalex/retro-go, 那里发布的是其它板子的固件, 对
// 本板不可用 —— 用户点"Check for updates"会下到刷不进去的东西。改指向本 fork。
// 注意: 本 fork 目前还没有发布任何 release, 所以这个菜单项在真正发版之前会报
// "Could not open releases URL" 或返回空列表, 这是预期的, 不是 bug。
#define RG_UPDATER_GITHUB_RELEASES "https://api.github.com/repos/lovely-him/retro-go/releases?per_page=10"

// 预设 Wi-Fi 网络。定义 RG_WIFI_DEFAULT_SSID 即可启用(RG_WIFI_DEFAULT_PASSWORD 可选,
// 开放网络不填), 具体行为见 rg_network.c 里 rg_network_init() 的那段 #ifdef。
//
// 作用只是"省去第一次在菜单里手填 SSID/密码"这一步: 仅当所选槽位里还没存过任何
// SSID 时才写入, 之后一切走正常路径 —— 用户在 Wi-Fi options 菜单里改过或存过别的
// 网络, 预设就永久不再介入, 不会被覆盖回去。
//
// 【注意】这两行是明文凭据, 会进 git 仓库; 而且落盘后也是明文, 存在
// /sd/retro-go/config/wifi.json(NS_WIFI -> "wifi", 见 rg_settings.c:23)。这是
// retro-go 本来的存储方式(菜单里手填也是存到同一个文件), 不是预设引入的新问题,
// 但如果这个仓库将来要公开, 记得先把这两行清掉。
//
// 不想改代码的话也有等价做法: 直接在电脑上编辑 SD 卡里的 wifi.json, 或者开机后按
// OPTION 进 Options -> Wi-Fi options -> Manage networks 手填一次。
#define RG_WIFI_DEFAULT_SSID        "public@sainFBB"
#define RG_WIFI_DEFAULT_PASSWORD    "sainM*VFFB"

// web 文件管理器的 httpd 任务栈大小(字节)。这是【参数】宏, 不是开关宏, 所以保留数值;
// 消费点在 launcher/main/webui.c 的 webui_start(), 用 #ifdef 包着, 未定义的 target
// 继续用 IDF 的默认值。
//
// 为什么必须改: IDF 5.5 的 HTTPD_DEFAULT_CONFIG() 把 .stack_size 硬编码成 4096
// (components/esp_http_server/include/esp_http_server.h:55), 而且 esp_http_server 的
// Kconfig 里【没有】对应选项(sdkconfig 里搜不到 CONFIG_HTTPD_DEFAULT_STACK_SIZE),
// 只能在代码里设。
//
// 4096 在本板会实打实地溢出。webui.c 的 http_api_handler 一次调用里叠了:
//   :48  char http_buffer[1024]        局部缓冲, 直接吃掉 1/4 的栈
//   :97  cJSON_Print(response)         递归打印
//        -> cJSON.c:623 sprintf("%1.15g", d)
//        -> newlib _svfprintf_r -> cvt -> _dtoa_r    dtoa 内部递归, 1~2KB
//   底下还有 FATFS/VFS 的 opendir/stat/readdir
// 实测崩在 _dtoa_r(dtoa.c:190), SP 0x4ff87710 已低于栈下界 0x4ff8776c,
// P4 硬件栈保护报 Stack protection fault(MCAUSE 0x1b)。
//
// 触发条件是数据相关的, 这也是上游一直没暴露的原因: cJSON_CreateNumber(cJSON.c:2505)
// 在 num >= INT_MAX 时把 valueint 饱和成 INT_MAX, 于是 print_number 里
// "d == (double)valueint" 不成立, 从便宜的 "%d" 掉进 "%1.15g" 这条浮点分支。
// 换句话说, 目录列表里只要有一个 size 或 mtime >= 2^31 就会走到 dtoa。
// 正常 ROM 几 MB、mtime 是 2026 年(1.79e9), 全都在 2^31 以下, 所以别的板子没事。
// 顺带一提这也是个 Y2038 bug: 2038-01-19 之后所有文件的 mtime 都会越过这条线。
//
// 8192 的代价是 launcher 多占 4KB 【内部】DRAM(httpd 的 task_caps 是
// MALLOC_CAP_INTERNAL, 栈不能放 PSRAM)。加 Wi-Fi 之后最大空闲内部块约 196KB, 够用。
// 只有 launcher 编译进 networking, 四个 core 不受影响。
#define RG_HTTPD_STACK_SIZE         8192
