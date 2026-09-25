#include "rg_system.h"

// 整个文件仅在 target 定义 RG_TOUCH_DRIVER_GT911 时参与编译, 与 drivers/audio/es8311.c
// 用 RG_AUDIO_USE_ES8311、drivers/input/usb_hid.c 用 RG_GAMEPAD_USB_MAP 门控是同一套
// 做法。没定义的 target 得到的是一个空翻译单元。
// 开关宏一律用"定义 / 不定义"表示, 不带数值: 这个能力内部没有分支要选, 用 0/1 配
// #ifdef 只会制造"把 1 改成 0 却关不掉"的陷阱。约定写在 target config.h 顶部。
#ifdef RG_TOUCH_DRIVER_GT911

#include "gt911.h"

// ── 实测: 本板的芯片自报是 GT927, 不是 GT911 ─────────────────────────────────
// 产品 ID(0x8140, 3 字节 ASCII)读到 39 32 37 = "927", 配置版本 0x82。厂商的 BSP 与
// board_check.c 都按 GT911 处理这块板(用的是 espressif/esp_lcd_touch_gt911), 芯片
// 自己不同意。
//
// 文件名与宏名仍沿用 gt911, 因为 Goodix GT9xx 家族共用这一套寄存器映射(0x8040 命令、
// 0x8047 配置、0x8140 产品 ID、0x814E 状态、0x814F 起每点 8 字节), 厂商驱动也是这样
// 一个 gt911 驱动覆盖全家的。四角实测已经逐条验证了映射正确, 见 rg_gt911_init() 的
// 日志与 config.h 的 Touch 段。如果将来换到寄存器映射不同的 GT9xx(如 GT9110 系列),
// 那才需要新驱动。

// ── 寄存器 ────────────────────────────────────────────────────────────────────
// GT9xx 的寄存器地址是 16 位、上线时高字节先发, 数据是小端。所以本文件一律走
// rg_i2c_read_reg16/write_reg16, 不能用只发一个地址字节的 rg_i2c_read/write。
#define GT911_REG_CMD           0x8040 // 写 0x05 进睡眠, 写 0x00 退出。本驱动绝不碰它, 原因见下
#define GT911_REG_CONFIG        0x8047 // [0]=版本号 [1..2]=X 输出分辨率 [3..4]=Y, 均小端
#define GT911_REG_PRODUCT_ID    0x8140 // 3 字节 ASCII: GT911 是 "911", 本板实测是 "927"
#define GT911_REG_STATUS        0x814E
#define GT911_REG_POINTS        0x814F // 紧随状态寄存器, 每点 8 字节

#define GT911_STATUS_BUF_READY  0x80
#define GT911_STATUS_KEY        0x10 // 置位表示这一帧是电容按键(0x8093)而不是坐标
#define GT911_STATUS_COUNT_MASK 0x0F

#define GT911_POINT_RECORD_LEN  8
#define GT911_PRODUCT_ID_LEN    3
#define GT911_CONFIG_LEN        5

// ── 为什么不做硬件复位、不写任何寄存器 ────────────────────────────────────────
// 本板的触摸 RST 与 INT 都是 GPIO_NUM_NC(厂商 board/esp32p4_wifi6_dev_kit.h 的
// BOARD_TOUCH_RST_GPIO / BOARD_TOUCH_INT_GPIO), 这带来两个硬约束:
//
// 1. 没有复位手段。厂商驱动 esp_lcd_touch_gt911 的 touch_gt911_reset() 整个函数体
//    被 if (rst_gpio_num != GPIO_NUM_NC) 包着, 在本板上就是个空操作; 它退而求其次
//    打的日志正是 "I2C address initialization procedure skipped - using default
//    GT9xx setup"。所以芯片里跑的是出厂烧录的那份 config, 我们只能顺着它用。
//
// 2. 绝不能写 0x8040 = 0x05(进睡眠)。唤醒靠的是拉 INT 脚: 厂商的 exit_sleep()
//    同样整个函数体被 if (int_gpio_num != GPIO_NUM_NC) 包着。本板 INT 是 NC, 一旦
//    进了睡眠就再也没有任何办法把芯片叫醒, 只能整机断电。因此本驱动是纯只读的。
//
// 代价是每次轮询都要读一次状态寄存器, 无法用中断唤醒。这是接线决定的, 不是设计选择。

// 连续失败到多少次就彻底停用。排线松了或触摸供电掉了会让每次 I2C 事务都失败, 而
// rg_input 以约 100Hz 轮询本驱动, rg_i2c 每次失败都打一条 RG_LOGE —— 几秒就能刷满
// 串口。停用并只报这一次, 比让日志把真正有用的信息冲掉要好。
#define GT911_MAX_ERRORS        20

// ── 探测为什么要重试 ──────────────────────────────────────────────────────────
// rg_input_init() 跑在 rg_display_init() 之前(见 rg_system.c 的初始化顺序), 而屏端
// 转接板的电源寄存器 0x95 是由显示驱动写的(mipi_dsi.h: dsi_panel_power_on())。触摸
// 芯片装在屏总成的同一块转接板上, 所以起初无法排除"0x95 也控制触摸供电轨"——厂商
// 代码里查不到答案。
//
// 已排除。2026-09-25 实测日志是 `gt911: at 0x5D (probe 1)`, 即在显示驱动还没跑、
// 0x95 还没写的时候第一次探测就命中了, 触摸供电与面板电源无关。
//
// 重试机制仍然保留, 但理由窄了很多: 那次是 CHIP_USB_UART_RESET(经调试口的热复位),
// 触摸芯片并没有跟着掉电重启。冷启动(拔插电源)时芯片要自己加载出厂 config 才开始
// 应答 I2C, 这个时间没有实测过。留 6 次 x 500ms = 3s 的窗口作为冷启动保险。
// 稳态开销: 找到之后每次 rg_gt911_read() 只多一个指针判空。
#define GT911_PROBE_RETRIES     6
#define GT911_PROBE_RETRY_US    500000

static uint8_t touch_addr = 0; // 0 = 还没找到 GT911(或已停用)
static uint8_t error_count = 0;
static int probe_attempts = 0;
static int64_t next_probe_time = 0;

// 统一的成功/失败登记点。所有 I2C 事务的返回值都过一遍这里。
static bool io_ok(bool success)
{
    if (success)
    {
        error_count = 0;
        return true;
    }
    if (++error_count >= GT911_MAX_ERRORS)
    {
        RG_LOGE("gt911: %d consecutive I2C failures at 0x%02X, touch disabled", error_count, touch_addr);
        touch_addr = 0;
        error_count = 0;
        // 一并掐掉重试: 探测重试是给"供电时序晚于 rg_input_init()"准备的, 不是给
        // 已经用过的设备掉了线准备的。否则这里会变成 探测->用->掉线->停用->再探测 的
        // 循环, 又把日志刷满。
        probe_attempts = GT911_PROBE_RETRIES;
    }
    return false;
}

bool rg_gt911_init(void)
{
    if (touch_addr)
        return true;

    // 重试次数用尽后直接返回, 一次总线访问都不做。这条路径在 rg_gt911_read() 里已经
    // 被 next_probe_time 挡住了, 这里是防止调用方(rg_input_init)重复调用时绕过它。
    if (probe_attempts >= GT911_PROBE_RETRIES)
        return false;

    ++probe_attempts;
    next_probe_time = rg_system_timer() + GT911_PROBE_RETRY_US;

    if (!rg_i2c_init())
        return false;

    // GT911 的地址由上电复位瞬间 INT 脚的电平决定(拉低 -> 0x5D, 拉高 -> 0x14)。
    // 本板 INT 是 NC, 电平由芯片内部决定, 所以两个都得探。顺序与厂商 BSP 的
    // bsp_touch_new() 一致。rg_i2c_probe() 只做一次地址握手, 不 add_device, 因此
    // 探不到的那个地址不会在 rg_i2c 的从机表里留下死槽位。
    uint8_t addr = 0;
    if (rg_i2c_probe(RG_TOUCH_I2C_ADDR))
        addr = RG_TOUCH_I2C_ADDR;
    else if (rg_i2c_probe(RG_TOUCH_I2C_ADDR_ALT))
        addr = RG_TOUCH_I2C_ADDR_ALT;

    if (!addr)
    {
        // 只在第一次和最后一次出声: 中间那几次重试注定失败(供电时序), 每次打一条
        // 只会把日志冲掉, 而它们不携带任何新信息。
        if (probe_attempts == 1)
            RG_LOGW("gt911: nothing at 0x%02X or 0x%02X, retrying (1/%d)", RG_TOUCH_I2C_ADDR,
                    RG_TOUCH_I2C_ADDR_ALT, GT911_PROBE_RETRIES);
        else if (probe_attempts >= GT911_PROBE_RETRIES)
            RG_LOGE("gt911: still nothing after %d attempts, touch disabled", probe_attempts);
        return false;
    }

    // 必须在首次事务之前登记速率: 新版 i2c_master API 的 SCL 速率在 add_device 时就
    // 固化了, 之后改不了。本板背光 0x45 与 ES8311 都跑 100kHz, 唯独触摸跑 400kHz ——
    // 厂商 BSP 也是这么分的(bsp_touch_new(): tp_io_config.scl_speed_hz =
    // CONFIG_BSP_I2C_CLK_SPEED_HZ = 400000)。同一条总线上两种速率是合法的:
    // i2c_master.c:702 在每次事务开始时按该设备的速率重设总线时序。
    rg_i2c_set_speed(addr, RG_TOUCH_I2C_SPEED);
    touch_addr = addr;

    uint8_t product_id[GT911_PRODUCT_ID_LEN];
    if (!io_ok(rg_i2c_read_reg16(addr, GT911_REG_PRODUCT_ID, product_id, sizeof(product_id))))
    {
        touch_addr = 0;
        return false;
    }

    // 厂商驱动只读产品 ID 和配置版本这两项(read_cfg), 这里多读 4 字节: 配置块紧跟着
    // 的 0x8048..0x804B 是 X/Y 输出分辨率(小端), 一次读完可以少一次事务。
    // 这两个偏移厂商源码里没有、只见于数据手册, 但已被实测证实: 读出 800x1280, 与
    // 四角实测的量程(X 5..791, Y 15..1277)以及面板分辨率三者吻合。
    uint8_t config[GT911_CONFIG_LEN] = {0};
    io_ok(rg_i2c_read_reg16(addr, GT911_REG_CONFIG, config, sizeof(config)));

    // probe N 是供电时序的判据: N > 1 就意味着触摸要等显示驱动写完 0x95 才有电。
    RG_LOGI("gt911: at 0x%02X (probe %d), id %02X %02X %02X, config ver 0x%02X, cfg res %dx%d, "
            "panel %dx%d",
            addr, probe_attempts, product_id[0], product_id[1], product_id[2], config[0],
            config[1] | (config[2] << 8), config[3] | (config[4] << 8), RG_TOUCH_PANEL_W,
            RG_TOUCH_PANEL_H);
    return true;
}

int rg_gt911_read(rg_touch_point_t *points, int max_points)
{
    if (!touch_addr)
    {
        // 还没找到(或已放弃)。到了重试时间点就再探一次, 见 rg_gt911_init() 的说明。
        if (probe_attempts >= GT911_PROBE_RETRIES || rg_system_timer() < next_probe_time)
            return -1;
        if (!rg_gt911_init())
            return -1;
    }

    uint8_t status;
    if (!io_ok(rg_i2c_read_reg16(touch_addr, GT911_REG_STATUS, &status, 1)))
        return -1;

    // bit7 为 0 表示缓冲区里没有新数据。此时【不需要】回写清零 —— 清零只是"我已取走
    // 数据"的应答, 没有数据可取。厂商驱动在这种情况下也写了一次, 那是多余的一半
    // 总线流量: 空闲时 100Hz 轮询会平白多出 100 次写事务。
    if (!(status & GT911_STATUS_BUF_READY))
        return 0;

    int count = status & GT911_STATUS_COUNT_MASK;

    // 本板屏上没有电容按键, bit4 置位时按"没有触点"处理。但清零照做, 否则控制器会
    // 一直停在同一帧上不再刷新。
    if (status & GT911_STATUS_KEY)
        count = 0;

    // 数据手册的上限就是 5, 所以 6..15 只可能是状态字节本身被读坏了(总线干扰或时序
    // 错位), 此时坐标大概率也是脏的。整帧丢弃, 但仍然要回写清零让控制器推进 ——
    // 与厂商驱动 read_data() 的 `touch_cnt > 5 || touch_cnt == 0` 分支一致。
    if (count > RG_TOUCH_MAX_POINTS)
        count = 0;

    int written = 0;
    if (count > 0 && points && max_points > 0)
    {
        written = RG_MIN(count, max_points);
        uint8_t buf[RG_TOUCH_MAX_POINTS * GT911_POINT_RECORD_LEN];
        if (io_ok(rg_i2c_read_reg16(touch_addr, GT911_REG_POINTS, buf, written * GT911_POINT_RECORD_LEN)))
        {
            for (int i = 0; i < written; ++i)
            {
                const uint8_t *rec = buf + (i * GT911_POINT_RECORD_LEN);
                // 每条记录: [0]=触点编号 [1..2]=X 小端 [3..4]=Y 小端 [5..6]=面积 [7]=保留
                points[i].id = rec[0];
                points[i].x = rec[1] | (rec[2] << 8);
                points[i].y = rec[3] | (rec[4] << 8);
            }
        }
        else
        {
            written = 0;
        }
    }

    // 清零必须在读完坐标之后: 提前写会让控制器在我们读的过程中就刷新缓冲区, 拿到
    // 半新半旧的一帧。count == 0(手指抬起那一帧)也要清, 这是唯一能让控制器推进的
    // 动作。写失败不影响本次返回的坐标, 只由 io_ok() 记账。
    io_ok(rg_i2c_write_reg16(touch_addr, GT911_REG_STATUS, 0x00));

    return written;
}

#endif // RG_TOUCH_DRIVER_GT911
