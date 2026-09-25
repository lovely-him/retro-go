// MIPI-DSI DPI 面板驱动 (RG_SCREEN_DRIVER == 2)
//
// 与 ili9341.h 的根本差异:
//   ILI9341/ST7789 是 SPI 屏, 自带 GRAM, MCU 把像素流式推过去即可, 因此那边的
//   lcd_send_buffer() 是排队一个异步 SPI 事务, lcd_sync() 等 DMA 排空。
//   DPI 面板没有 GRAM, SoC 必须持续从自己内存里的帧缓冲扫描输出, 因此本驱动
//   持有帧缓冲, lcd_send_buffer() 是一次同步的内存写入, lcd_sync() 无事可做。
//
// 本驱动额外承担两件事:
//   1. 逻辑分辨率 -> 面板分辨率的整数倍放大与定位。RG_SCREEN_WIDTH/HEIGHT 是逻辑值
//      (见 target config.h), 面板物理分辨率来自 jd9365_waveshare_10p1.h。倍数只允许
//      整数, 内容水平居中、垂直顶部对齐, 面板其余部分保持黑色。
//      整数倍是硬要求: 点阵字体在非整数倍最近邻放大下, 1px 笔画会时而 2px 时而 3px,
//      同一行文字粗细不均, 实测辨识度明显变差。
//   2. RGB565 大端 -> 小端。retro-go 全链路用大端(为 SPI 屏的字节发送顺序服务),
//      而内存映射的 DPI 帧缓冲要求原生小端。转换放在这里是刻意的取舍: 放大本身
//      已经是逐像素循环, 多一条 rotate 不增加额外的内存遍历; 若改为全链路小端,
//      需要动 rg_display.c 与 8 个 core 的 port 文件及其调色板构造, 与"尽量不改
//      上游、只增不改"的原则冲突。
//      实测结论: launcher 场景 BUSY 1% / FPS 108, 这条转换完全不是瓶颈,
//      全链路改小端的重构没有必要做。

#pragma once

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_cache.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "jd9365_waveshare_10p1.h"

// DCS 标准命令(用字面值, 避免依赖 esp_lcd 的宏名)
#define DSI_CMD_SWRESET     0x01
#define DSI_CMD_RDDID       0x04
#define DSI_CMD_MADCTL      0x36
#define DSI_CMD_COLMOD      0x3A

// MADCTL = 0x00 表示 RGB 子像素顺序、无翻转
#define DSI_MADCTL_RGB      0x00

// 整数倍放大 + 面板内定位。倍数由 target 的 config.h 给定(只允许整数, 理由见该文件)。
// 偏移【也】由 config.h 给定(RG_DSI_OFFSET_X/Y): 触摸模块要做面板->逻辑坐标的换算,
// 但本文件只被 rg_display.c 按 RG_SCREEN_DRIVER 选中, 触摸模块看不到它, 所以偏移
// 只能放在两边都可见的 config.h 里。下面的 #if 校验它与"水平居中、垂直顶部对齐"的
// 算法一致, 两处不可能漂移。
// 面板物理分辨率来自 jd9365_waveshare_10p1.h, 逻辑分辨率即 RG_SCREEN_WIDTH/HEIGHT。
#define DSI_SCALE_X             RG_DSI_SCALE_X
#define DSI_SCALE_Y             RG_DSI_SCALE_Y
#define DSI_CONTENT_W           (RG_SCREEN_WIDTH  * DSI_SCALE_X)
#define DSI_CONTENT_H           (RG_SCREEN_HEIGHT * DSI_SCALE_Y)
#define DSI_OFFSET_X            RG_DSI_OFFSET_X
#define DSI_OFFSET_Y            RG_DSI_OFFSET_Y

#define DSI_LOGIC_TO_PANEL_X(x)  (DSI_OFFSET_X + (x) * DSI_SCALE_X)
#define DSI_LOGIC_TO_PANEL_Y(y)  (DSI_OFFSET_Y + (y) * DSI_SCALE_Y)

#if DSI_CONTENT_W > RG_DSI_PANEL_H_RES || DSI_CONTENT_H > RG_DSI_PANEL_V_RES
#error "RG_SCREEN_WIDTH/HEIGHT x RG_DSI_SCALE 超出面板物理分辨率, 请减小逻辑分辨率或缩放倍数"
#endif
#if DSI_OFFSET_X != ((RG_DSI_PANEL_H_RES - DSI_CONTENT_W) / 2)
#error "RG_DSI_OFFSET_X 与 (面板宽 - 内容宽)/2 不一致, 请同步更新 config.h"
#endif
#if DSI_OFFSET_Y != 0
#error "RG_DSI_OFFSET_Y 必须为 0: 内容垂直顶部对齐, 这是本 target 的定位规则"
#endif

static esp_lcd_dsi_bus_handle_t dsi_bus;
static esp_lcd_panel_io_handle_t dsi_io;
static esp_lcd_panel_handle_t dsi_panel;
static esp_ldo_channel_handle_t dsi_phy_ldo;
static bool disp_i2c_ready;   // 屏端电源/背光控制器是否已可用

static uint16_t *panel_fb;                        // 面板帧缓冲, PSRAM, 由 IDF 分配
static uint16_t lcd_buffer[LCD_BUFFER_LENGTH];    // 条带缓冲, 内部 RAM

static struct
{
    int left, top, width, height;   // 当前窗口, 逻辑坐标
    int cursor;                     // 窗口内已消费的【像素】数(不是行数)
} dsi_window;

// I2C 上挂的是屏端转接板的电源/背光控制器(地址 RG_DISP_I2C_ADDR), 不只是背光:
// 寄存器 RG_DISP_REG_POWER 负责给面板上电, RG_DISP_REG_BACKLIGHT 才是亮度。
//
// 这条总线【不归本驱动所有】: 它和 ES8311 codec、GT911 触摸共用, 由 rg_i2c.c 统一
// 持有(见 target config.h 的 "I2C 总线(板载, 共享)" 一节)。同一端口建第二条
// i2c_new_master_bus() 会失败, 所以这里只登记自己的速率然后使用。
//
// 速率 100kHz: 板级头文件声称板载器件均为 400kHz, 但厂商面板驱动实测用 100kHz,
// 以能跑通的为准。新版 i2c_master API 按设备设速率, 因此不影响同总线上的其它器件。
static bool dsi_disp_i2c_init(void)
{
    // 先登记速率再 init: rg_i2c 的 device handle 是首次访问时才按登记值懒创建的,
    // 所以顺序其实无所谓, 但这样写读起来是"声明需求 -> 准备总线"。
    rg_i2c_set_speed(RG_DISP_I2C_ADDR, RG_DISP_I2C_SPEED);

    if (!rg_i2c_init())
    {
        RG_LOGE("disp i2c: bus init failed");
        return false;
    }

    disp_i2c_ready = true;
    return true;
}

static bool dsi_disp_write_reg(uint8_t reg, uint8_t value)
{
    if (!rg_i2c_write_byte(RG_DISP_I2C_ADDR, reg, value))
    {
        RG_LOGE("disp i2c: write reg 0x%02X = 0x%02X failed", reg, value);
        return false;
    }
    return true;
}

// 面板上电时序。必须在创建 DPI 面板、发任何 DCS 命令之前完成, 否则面板不响应,
// 而 DSI 主机的硬件超时被 IDF 显式禁用(esp_lcd_mipi_dsi_bus.c 里
// set_timeout_count 全传 0), 一次读命令就会永久自旋, 表现为 main 任务忙等、
// IDLE0 饿死、task_wdt 每 5 秒无限重复。
static bool dsi_panel_power_on(void)
{
    if (!dsi_disp_i2c_init())
        return false;

    bool ok = dsi_disp_write_reg(RG_DISP_REG_POWER, 0x11);
    ok = dsi_disp_write_reg(RG_DISP_REG_POWER, 0x17) && ok;
    ok = dsi_disp_write_reg(RG_DISP_REG_BACKLIGHT, 0x00) && ok;
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = dsi_disp_write_reg(RG_DISP_REG_BACKLIGHT, 0xFF) && ok;
    vTaskDelay(pdMS_TO_TICKS(1000));   // 厂商驱动在此处整秒延时, 面板需要稳定时间
    return ok;
}

static void lcd_init(void)
{
    // D-PHY 供电必须先于建总线, 否则 PHY 无法从 No Power 进入 Shutdown 状态
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = RG_DSI_PHY_LDO_CHAN,
        .voltage_mv = RG_DSI_PHY_LDO_MV,
    };
    if (esp_ldo_acquire_channel(&ldo_cfg, &dsi_phy_ldo) != ESP_OK)
    {
        RG_LOGE("DSI PHY LDO chan %d at %d mV failed", RG_DSI_PHY_LDO_CHAN, RG_DSI_PHY_LDO_MV);
        return;
    }

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = RG_DSI_PANEL_LANE_NUM,
        .phy_clk_src = 0,   // 与厂商 BSP 一致, 用默认 PLL 参考时钟
        .lane_bit_rate_mbps = RG_DSI_PANEL_LANE_MBPS,
    };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK)
    {
        RG_LOGE("new DSI bus failed");
        return;
    }

    // DBI 通道只用来发 DCS 命令, 像素数据走 DPI
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dsi_io) != ESP_OK)
    {
        RG_LOGE("new DBI panel io failed");
        return;
    }

    // 面板上电必须在此处完成: 早于 DPI 面板创建, 更早于任何 DCS 命令。
    // 顺序照搬厂商面板驱动(经 BSP 验证可点亮本屏)。失败就不要继续 —— 面板没上电时
    // 后面的 DCS 读会因为 DSI 超时被禁用而永久自旋。
    if (!dsi_panel_power_on())
    {
        RG_LOGE("panel power-on sequence failed, aborting display init");
        return;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = RG_DSI_PANEL_DPI_CLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,   // 单缓冲, 暂不抗撕裂
        .video_timing = {
            .h_size = RG_DSI_PANEL_H_RES,
            .v_size = RG_DSI_PANEL_V_RES,
            .hsync_pulse_width = RG_DSI_PANEL_HSYNC,
            .hsync_back_porch = RG_DSI_PANEL_HBP,
            .hsync_front_porch = RG_DSI_PANEL_HFP,
            .vsync_pulse_width = RG_DSI_PANEL_VSYNC,
            .vsync_back_porch = RG_DSI_PANEL_VBP,
            .vsync_front_porch = RG_DSI_PANEL_VFP,
        },
        // IDF 5.5 的 DPI 驱动对 use_dma2d 直接返回 ESP_ERR_NOT_SUPPORTED
        // (esp_lcd_panel_dpi.c:174), 必须为 0。
        .flags.use_dma2d = 0,
    };
    if (esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &dsi_panel) != ESP_OK)
    {
        RG_LOGE("new DPI panel failed");
        return;
    }

    if (esp_lcd_dpi_panel_get_frame_buffer(dsi_panel, 1, (void **)&panel_fb) != ESP_OK || !panel_fb)
    {
        RG_LOGE("get frame buffer failed");
        panel_fb = NULL;
        return;
    }
    RG_LOGI("frame buffer %p, %d KB", panel_fb,
            RG_DSI_PANEL_H_RES * RG_DSI_PANEL_V_RES * 2 / 1024);
    RG_LOGI("layout: logical %dx%d x%d -> content %dx%d at (%d,%d) on panel %dx%d",
            RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, DSI_SCALE_X, DSI_CONTENT_W, DSI_CONTENT_H,
            DSI_OFFSET_X, DSI_OFFSET_Y, RG_DSI_PANEL_H_RES, RG_DSI_PANEL_V_RES);

    // 内容区只占面板一部分, 而 rg_display 只写脏区域、从不碰边框, 所以边框必须由
    // 我们清成黑色。IDF 用 heap_caps_calloc 分配帧缓冲本身已是 0, 这里显式清一次
    // 是为了不依赖分配器的清零行为。
    memset(panel_fb, 0, (size_t)RG_DSI_PANEL_H_RES * RG_DSI_PANEL_V_RES * sizeof(uint16_t));
    esp_cache_msync(panel_fb, (size_t)RG_DSI_PANEL_H_RES * RG_DSI_PANEL_V_RES * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // 本板 RST 未接 GPIO(板级头文件 BOARD_LCD_RST_GPIO = NC), 只能软复位
    esp_lcd_panel_io_tx_param(dsi_io, DSI_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t id[3] = {0};
    if (esp_lcd_panel_io_rx_param(dsi_io, DSI_CMD_RDDID, id, 3) == ESP_OK)
        RG_LOGI("panel ID: %02X %02X %02X (expect 93 65 04)", id[0], id[1], id[2]);

    uint8_t page_user = RG_DSI_PAGE_USER;
    uint8_t madctl = DSI_MADCTL_RGB;
    uint8_t colmod = RG_DSI_COLMOD_RGB565;
    uint8_t lanes = RG_DSI_2_LANE;
    esp_lcd_panel_io_tx_param(dsi_io, RG_DSI_CMD_PAGE, &page_user, 1);
    esp_lcd_panel_io_tx_param(dsi_io, DSI_CMD_MADCTL, &madctl, 1);
    esp_lcd_panel_io_tx_param(dsi_io, DSI_CMD_COLMOD, &colmod, 1);
    esp_lcd_panel_io_tx_param(dsi_io, RG_DSI_CMD_DSI_INT0, &lanes, 1);

    for (int i = 0; i < RG_DSI_INIT_SEQ_LEN; ++i)
    {
        uint8_t param = rg_dsi_init_seq[i][1];
        if (esp_lcd_panel_io_tx_param(dsi_io, rg_dsi_init_seq[i][0], &param, 1) != ESP_OK)
        {
            RG_LOGE("init seq failed at entry %d (cmd 0x%02X)", i, rg_dsi_init_seq[i][0]);
            return;
        }
        if (rg_dsi_init_seq[i][2])
            vTaskDelay(pdMS_TO_TICKS(rg_dsi_init_seq[i][2]));
    }

    // DCS 序列发完才启动 DPI 视频流, 与厂商驱动的时序一致
    if (esp_lcd_panel_init(dsi_panel) != ESP_OK)
        RG_LOGE("DPI panel init failed");

    // I2C 与背光已在 dsi_panel_power_on() 里于上电时序中完成, 此处无需再初始化
}

static void lcd_deinit(void)
{
    // 不动 I2C: 总线由 rg_i2c.c 持有且与 ES8311/GT911 共享, 显示模块无权拆除它。
    disp_i2c_ready = false;
    if (dsi_panel)
    {
        esp_lcd_panel_del(dsi_panel);
        dsi_panel = NULL;
    }
    if (dsi_io)
    {
        esp_lcd_panel_io_del(dsi_io);
        dsi_io = NULL;
    }
    if (dsi_bus)
    {
        esp_lcd_del_dsi_bus(dsi_bus);
        dsi_bus = NULL;
    }
    if (dsi_phy_ldo)
    {
        esp_ldo_release_channel(dsi_phy_ldo);
        dsi_phy_ldo = NULL;
    }
    panel_fb = NULL;
}

// 写入是同步完成的, 没有待排空的事务
static void lcd_sync(void)
{
}

static void lcd_set_backlight(float percent)
{
    if (!disp_i2c_ready)
        return;
    if (percent < 0.f) percent = 0.f;
    if (percent > 1.f) percent = 1.f;
    dsi_disp_write_reg(RG_DISP_REG_BACKLIGHT, (uint8_t)(255.f * percent));
}

// 契约(见 rg_display.c 的三个调用方):
//   - lcd_set_window 的 height 传的是"从 top 起的剩余总行数", 不是本次条带行数;
//     之后连续的 lcd_send_buffer 不再调它, 由驱动自己推进窗口内的游标。
//   - lcd_send_buffer 的 length 是【平坦像素数】, 不保证是 width 的整数倍。
//     write_update 传 draw_width*lines(行结构化), 但 rg_display_clear_rect 传的是
//     min(剩余像素, LCD_BUFFER_LENGTH), 与 width 没有整除关系。ili9341 靠面板 GRAM
//     在窗口内自动递增地址, 平坦流天然正确; 本驱动必须自己按窗口行宽分解游标,
//     否则余数被丢弃、游标错位, 纯色填充会漂移成一条条色带。
static void lcd_set_window(int left, int top, int width, int height)
{
    dsi_window.left = left;
    dsi_window.top = top;
    dsi_window.width = width;
    dsi_window.height = height;
    dsi_window.cursor = 0;   // 像素游标, 不是行游标
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    // 调用方是严格串行的(get -> fill -> send -> get ...), 且本驱动的 send 是同步
    // 消费, 返回后即可复用, 因此单个静态缓冲足够, 不需要 ili9341 那样的缓冲池。
    return lcd_buffer;
}

// 把一个逻辑行内的连续像素段放大写入面板。
// 数据在 src[0..count), 对应逻辑坐标 (logic_top, logic_left) 起的 count 个像素。
// 整数倍放大: 每个逻辑像素横向复制 SCALE_X 份, 该行再纵向复制 SCALE_Y 份。
static void dsi_blit_span(const uint16_t *src, int logic_top, int logic_left, int count)
{
    const int px0 = DSI_LOGIC_TO_PANEL_X(logic_left);
    const int py0 = DSI_LOGIC_TO_PANEL_Y(logic_top);
    const int px1 = px0 + count * DSI_SCALE_X;
    const int py1 = py0 + DSI_SCALE_Y;

    // 窗口越界时整段丢弃。正常不会发生(rg_display 的窗口恒在逻辑屏内),
    // 但一旦算错就是写穿帧缓冲, 宁可整段不画。
    if (count < 1 || px0 < 0 || py0 < 0 || px1 > RG_DSI_PANEL_H_RES || py1 > RG_DSI_PANEL_V_RES)
        return;

    uint16_t *row0 = panel_fb + (size_t)py0 * RG_DSI_PANEL_H_RES + px0;
    for (int i = 0; i < count; ++i)
    {
        const uint16_t v = src[i];
        const uint16_t le = (uint16_t)((v << 8) | (v >> 8));   // retro-go 大端 -> 帧缓冲原生小端
        for (int k = 0; k < DSI_SCALE_X; ++k)
            row0[i * DSI_SCALE_X + k] = le;
    }

    // 纵向其余行与首行完全相同, 整行复制
    for (int py = py0 + 1; py < py1; ++py)
        memcpy(panel_fb + (size_t)py * RG_DSI_PANEL_H_RES + px0, row0,
               (size_t)(px1 - px0) * sizeof(uint16_t));

    // 帧缓冲在 PSRAM, CPU 经 L2 cache 写入, 必须回刷 DSI 桥才看得到。
    // 按整行跨度回刷而非仅脏列: 干净 cache line 的回刷不产生总线流量, 换来的是
    // 每个 span 只需一次 msync 调用。
    esp_cache_msync(panel_fb + (size_t)py0 * RG_DSI_PANEL_H_RES,
                    (size_t)(py1 - py0) * RG_DSI_PANEL_H_RES * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    // length == 0 表示调用方归还缓冲但无数据(write_update 里未变化的条带走这条)
    if (length == 0 || !panel_fb || dsi_window.width < 1 || dsi_window.height < 1)
        return;

    const int win_pixels = dsi_window.width * dsi_window.height;
    size_t pos = 0;
    while (pos < length)
    {
        if (dsi_window.cursor >= win_pixels)
            break;   // 调用方给的像素数超出窗口容量, 截断而非写穿

        const int row = dsi_window.cursor / dsi_window.width;
        const int col = dsi_window.cursor % dsi_window.width;

        size_t n = (size_t)(dsi_window.width - col);          // 本行还能放多少
        if (n > length - pos) n = length - pos;               // 不超过本次数据量
        if (n > (size_t)(win_pixels - dsi_window.cursor))     // 不超过窗口容量
            n = (size_t)(win_pixels - dsi_window.cursor);

        dsi_blit_span(buffer + pos, dsi_window.top + row, dsi_window.left + col, (int)n);
        pos += n;
        dsi_window.cursor += (int)n;
    }
}

const rg_display_driver_t rg_display_driver_mipi_dsi = {
    .name = "mipi-dsi",
};
