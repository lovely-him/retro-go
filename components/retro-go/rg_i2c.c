#include "rg_system.h"
#include "rg_i2c.h"

#include <stdlib.h>
#include <string.h>

// 两条互斥的实现路径, RG_I2C_MASTER_DRIVER 优先。
// 新版 i2c_master API 的 SCL 速率是【每设备】设置的, 同一条总线上不同器件可以各跑
// 各的速率; legacy driver/i2c.h 只有总线级速率, 做不到。本板确实需要这个能力: 厂商
// 代码把背光控制器 0x45 和 ES8311 都跑在 100kHz, 唯独 GT911 触摸用可配置的
// CONFIG_BSP_I2C_CLK_SPEED_HZ, 所以将来接触摸时同一总线上会出现两种速率。
#if defined(ESP_PLATFORM) && defined(RG_I2C_MASTER_DRIVER)
#include <driver/i2c_master.h>
#define USE_I2C_MASTER_DRIVER 1
#elif defined(ESP_PLATFORM) && defined(RG_GPIO_I2C_SDA) && defined(RG_GPIO_I2C_SCL)
#include <driver/i2c.h>
#include <esp_err.h>
#define USE_I2C_DRIVER 1
#endif

#if USE_I2C_MASTER_DRIVER
#ifndef RG_I2C_PORT
#define RG_I2C_PORT I2C_NUM_0
#endif
#ifndef RG_I2C_SPEED
#define RG_I2C_SPEED 400000       // 未经 rg_i2c_set_speed() 登记的地址用这个
#endif
#ifndef RG_I2C_INTERNAL_PULLUP
#define RG_I2C_INTERNAL_PULLUP 1  // 与 legacy 分支的 GPIO_PULLUP_ENABLE 一致
#endif
#ifndef RG_I2C_TIMEOUT_MS
#define RG_I2C_TIMEOUT_MS 500     // 与 legacy 分支的 pdMS_TO_TICKS(500) 一致
#endif
// reg 前缀与数据要合并成一个连续缓冲再发送, 这是它的上限。本仓库所有调用方都远小于
// 此值(RTC 7 字节、I2C GPIO 扩展器 2 字节、codec 与背光各 2 字节)。超出直接报错,
// 不静默截断。
#define RG_I2C_WRITE_MAX 32
// 可登记的从机数量。本板: 背光 0x45 + ES8311 0x18 + GT911(0x5D 与 0x14 二选一, 先
// 用 rg_i2c_probe() 探到哪个再登记哪个, 不会两个都占位) = 3, 余 1 个空位。
#define RG_I2C_MAX_DEVICES 4
#endif

static bool i2c_initialized = false;

#define TRY(x)                 \
    if ((err = (x)) != ESP_OK) \
    {                          \
        goto fail;             \
    }

#if USE_I2C_MASTER_DRIVER
static i2c_master_bus_handle_t i2c_bus;

// 从机表。handle 懒创建: rg_i2c_set_speed() 只登记速率, 首次访问该地址时才真正
// i2c_master_bus_add_device(), 这样调用方不必关心"登记"与"总线就绪"的先后顺序。
static struct
{
    uint8_t addr;
    uint32_t speed_hz;
    i2c_master_dev_handle_t handle;
} i2c_devices[RG_I2C_MAX_DEVICES];
static size_t i2c_devices_count;

static i2c_master_dev_handle_t i2c_get_device(uint8_t addr)
{
    size_t slot = i2c_devices_count; // 没找到时, 这就是新槽位
    for (size_t i = 0; i < i2c_devices_count; ++i)
    {
        if (i2c_devices[i].addr == addr)
        {
            slot = i;
            break;
        }
    }

    if (slot == i2c_devices_count)
    {
        if (slot >= RG_I2C_MAX_DEVICES)
        {
            RG_LOGE("I2C: device table full (max %d), cannot add 0x%02X", RG_I2C_MAX_DEVICES, addr);
            return NULL;
        }
        i2c_devices[slot].addr = addr;
        i2c_devices[slot].speed_hz = RG_I2C_SPEED;
        i2c_devices[slot].handle = NULL;
        i2c_devices_count = slot + 1;
    }

    if (i2c_devices[slot].handle)
        return i2c_devices[slot].handle;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = i2c_devices[slot].speed_hz,
    };
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_devices[slot].handle) != ESP_OK)
    {
        RG_LOGE("I2C: add device 0x%02X @ %uHz failed", addr, (unsigned)i2c_devices[slot].speed_hz);
        i2c_devices[slot].handle = NULL;
    }
    return i2c_devices[slot].handle;
}

bool rg_i2c_set_speed(uint8_t addr, uint32_t speed_hz)
{
    if (speed_hz == 0)
        return false;

    for (size_t i = 0; i < i2c_devices_count; ++i)
    {
        if (i2c_devices[i].addr != addr)
            continue;
        if (i2c_devices[i].handle)
        {
            // 新版 API 的速率在 add_device 时就固化了, 改不了
            RG_LOGW("I2C: 0x%02X already in use, speed stays %uHz", addr, (unsigned)i2c_devices[i].speed_hz);
            return false;
        }
        i2c_devices[i].speed_hz = speed_hz;
        return true;
    }

    if (i2c_devices_count >= RG_I2C_MAX_DEVICES)
    {
        RG_LOGE("I2C: device table full (max %d), cannot register 0x%02X", RG_I2C_MAX_DEVICES, addr);
        return false;
    }
    i2c_devices[i2c_devices_count].addr = addr;
    i2c_devices[i2c_devices_count].speed_hz = speed_hz;
    i2c_devices[i2c_devices_count].handle = NULL;
    i2c_devices_count++;
    return true;
}

bool rg_i2c_probe(uint8_t addr)
{
    if (!i2c_initialized)
    {
        RG_LOGE("Probe 0x%02X failed: bus not initialized\n", addr);
        return false;
    }
    // i2c_master_probe() 走总线锁做一次纯地址握手, 不 add_device, 所以探测一个不存在
    // 的地址不会在 i2c_devices[] 里留下死槽位。100ms 超时与厂商 BSP 的
    // bsp_i2c_device_probe() 取值一致。
    return i2c_master_probe(i2c_bus, addr, 100) == ESP_OK;
}
#else
bool rg_i2c_set_speed(uint8_t addr, uint32_t speed_hz)
{
    // legacy 驱动只有总线级速率, 无法按设备设置
    RG_LOGW("I2C: per-device speed not supported by this driver (addr 0x%02X, %uHz ignored)", addr,
            (unsigned)speed_hz);
    return false;
}

bool rg_i2c_probe(uint8_t addr)
{
    RG_LOGW("I2C: probe not supported by this driver (addr 0x%02X)\n", addr);
    return false;
}
#endif


bool rg_i2c_init(void)
{
#if USE_I2C_MASTER_DRIVER
    if (i2c_initialized)
        return true;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = RG_I2C_PORT,
        .sda_io_num = RG_GPIO_I2C_SDA,
        .scl_io_num = RG_GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = RG_I2C_INTERNAL_PULLUP,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus);
    if (err != ESP_OK)
    {
        RG_LOGE("I2C bus init failed (port:%d SDA:%d SCL:%d) err=0x%x", RG_I2C_PORT, RG_GPIO_I2C_SDA,
                RG_GPIO_I2C_SCL, err);
        i2c_bus = NULL;
        i2c_initialized = false;
        return false;
    }

    RG_LOGI("I2C bus ready (port:%d SDA:%d SCL:%d pullup:%d).\n", RG_I2C_PORT, RG_GPIO_I2C_SDA, RG_GPIO_I2C_SCL,
            RG_I2C_INTERNAL_PULLUP);
    i2c_initialized = true;
    return true;
#elif USE_I2C_DRIVER
    const i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RG_GPIO_I2C_SDA,
        .scl_io_num = RG_GPIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = ESP_FAIL;

    if (i2c_initialized)
        return true;

    TRY(i2c_param_config(I2C_NUM_0, &i2c_config));
    TRY(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
    RG_LOGI("I2C driver ready (SDA:%d SCL:%d).\n", i2c_config.sda_io_num, i2c_config.scl_io_num);
    i2c_initialized = true;
    return true;
fail:
    RG_LOGE("Failed to initialize I2C driver. err=0x%x\n", err);
#else
    RG_LOGE("I2C is not available on this device.\n");
#endif
    i2c_initialized = false;
    return false;
}

bool rg_i2c_deinit(void)
{
#if USE_I2C_MASTER_DRIVER
    if (i2c_initialized)
    {
        for (size_t i = 0; i < i2c_devices_count; ++i)
        {
            if (i2c_devices[i].handle)
            {
                i2c_master_bus_rm_device(i2c_devices[i].handle);
                i2c_devices[i].handle = NULL;
            }
        }
        i2c_devices_count = 0;
        if (i2c_del_master_bus(i2c_bus) == ESP_OK)
            RG_LOGI("I2C bus terminated.\n");
        i2c_bus = NULL;
    }
#elif USE_I2C_DRIVER
    if (i2c_initialized && i2c_driver_delete(I2C_NUM_0) == ESP_OK)
        RG_LOGI("I2C driver terminated.\n");
#endif
    i2c_initialized = false;
    return true;
}

bool rg_i2c_read(uint8_t addr, int reg, void *read_data, size_t read_len)
{
#if USE_I2C_MASTER_DRIVER
    if (!i2c_initialized)
    {
        RG_LOGE("Read from 0x%02X failed: bus not initialized\n", addr);
        return false;
    }

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev)
        return false;

    // reg < 0 表示"不带寄存器地址, 直接读"(PCF857x 这类扩展器需要)
    esp_err_t err;
    if (reg >= 0)
    {
        const uint8_t r = (uint8_t)reg;
        err = i2c_master_transmit_receive(dev, &r, 1, read_data, read_len, RG_I2C_TIMEOUT_MS);
    }
    else
    {
        err = i2c_master_receive(dev, read_data, read_len, RG_I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK)
    {
        RG_LOGE("Read from 0x%02X failed. reg=0x%02X, err=0x%03X\n", addr, reg, err);
        return false;
    }
    return true;
#elif USE_I2C_DRIVER
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err = ESP_FAIL;

    if (!cmd || !i2c_initialized)
        goto fail;

    if (reg >= 0)
    {
        TRY(i2c_master_start(cmd));
        TRY(i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true));
        TRY(i2c_master_write_byte(cmd, (uint8_t)reg, true));
    }
    TRY(i2c_master_start(cmd));
    TRY(i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true));
    TRY(i2c_master_read(cmd, read_data, read_len, I2C_MASTER_LAST_NACK));
    TRY(i2c_master_stop(cmd));
    TRY(i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(500)));
    i2c_cmd_link_delete(cmd);
    return true;
fail:
    i2c_cmd_link_delete(cmd);
    RG_LOGE("Read from 0x%02X failed. reg=0x%02X, err=0x%03X, init=%d\n", addr, reg, err, i2c_initialized);
#endif
    return false;
}

bool rg_i2c_write(uint8_t addr, int reg, const void *write_data, size_t write_len)
{
#if USE_I2C_MASTER_DRIVER
    if (!i2c_initialized)
    {
        RG_LOGE("Write to 0x%02X failed: bus not initialized\n", addr);
        return false;
    }

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev)
        return false;

    const uint8_t *buffer = write_data;
    size_t buffer_len = write_len;
    uint8_t prefixed[RG_I2C_WRITE_MAX];

    // 新 API 的 i2c_master_transmit 只发一个连续缓冲, 所以 reg 前缀要先拼上去
    if (reg >= 0)
    {
        if (write_len + 1 > sizeof(prefixed))
        {
            RG_LOGE("Write to 0x%02X failed: %u bytes exceeds RG_I2C_WRITE_MAX (%d)\n", addr,
                    (unsigned)(write_len + 1), RG_I2C_WRITE_MAX);
            return false;
        }
        prefixed[0] = (uint8_t)reg;
        memcpy(prefixed + 1, write_data, write_len);
        buffer = prefixed;
        buffer_len = write_len + 1;
    }

    esp_err_t err = i2c_master_transmit(dev, buffer, buffer_len, RG_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        RG_LOGE("Write to 0x%02X failed. reg=0x%02X, err=0x%03X\n", addr, reg, err);
        return false;
    }
    return true;
#elif USE_I2C_DRIVER
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err = ESP_FAIL;

    if (!cmd || !i2c_initialized)
        goto fail;

    TRY(i2c_master_start(cmd));
    TRY(i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true));
    if (reg >= 0)
    {
        TRY(i2c_master_write_byte(cmd, (uint8_t)reg, true));
    }
    TRY(i2c_master_write(cmd, (void *)write_data, write_len, true));
    TRY(i2c_master_stop(cmd));
    TRY(i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(500)));
    i2c_cmd_link_delete(cmd);
    return true;
fail:
    i2c_cmd_link_delete(cmd);
    RG_LOGE("Write to 0x%02X failed. reg=0x%02X, err=0x%03X, init=%d\n", addr, reg, err, i2c_initialized);
#endif
    return false;
}

int rg_i2c_read_byte(uint8_t addr, uint8_t reg)
{
    uint8_t value;
    return rg_i2c_read(addr, reg, &value, 1) ? value : -1;
}

bool rg_i2c_write_byte(uint8_t addr, uint8_t reg, uint8_t value)
{
    return rg_i2c_write(addr, reg, &value, 1);
}

// 16-bit 寄存器地址版本。GT911 触摸控制器的寄存器地址是 16 位、大端上线(高字节先
// 发), 而上面的 rg_i2c_read/write 只发一个地址字节, 覆盖不到。只有新版 i2c_master
// 分支实现: legacy driver/i2c.h 那条路径上没有需要 16-bit 寻址的器件。
bool rg_i2c_read_reg16(uint8_t addr, uint16_t reg, void *read_data, size_t read_len)
{
#if USE_I2C_MASTER_DRIVER
    if (!i2c_initialized)
    {
        RG_LOGE("Read from 0x%02X failed: bus not initialized\n", addr);
        return false;
    }

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev)
        return false;

    const uint8_t reg_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    esp_err_t err = i2c_master_transmit_receive(dev, reg_be, sizeof(reg_be), read_data, read_len, RG_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        RG_LOGE("Read from 0x%02X failed. reg16=0x%04X, err=0x%03X\n", addr, reg, err);
        return false;
    }
    return true;
#else
    RG_LOGE("I2C: 16-bit register addressing requires RG_I2C_MASTER_DRIVER (addr 0x%02X reg 0x%04X)\n", addr, reg);
    return false;
#endif
}

bool rg_i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t value)
{
#if USE_I2C_MASTER_DRIVER
    if (!i2c_initialized)
    {
        RG_LOGE("Write to 0x%02X failed: bus not initialized\n", addr);
        return false;
    }

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev)
        return false;

    const uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    esp_err_t err = i2c_master_transmit(dev, buf, sizeof(buf), RG_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        RG_LOGE("Write to 0x%02X failed. reg16=0x%04X val=0x%02X, err=0x%03X\n", addr, reg, value, err);
        return false;
    }
    return true;
#else
    RG_LOGE("I2C: 16-bit register addressing requires RG_I2C_MASTER_DRIVER (addr 0x%02X reg 0x%04X)\n", addr, reg);
    return false;
#endif
}


#ifdef RG_I2C_GPIO_DRIVER

typedef struct {int input_reg, output_reg, direction_reg, pullup_reg;} _gpio_port;
typedef struct {uint8_t reg, value;} _gpio_sequence;

#if RG_I2C_GPIO_DRIVER == 1 // AW9523

static const _gpio_port gpio_ports[] = {
    {0x00, 0x02, 0x04, -1}, // PORT 0
    {0x01, 0x03, 0x05, -1}, // PORT 1
};
static const _gpio_sequence gpio_init_seq[] = {
    {0x7F, 0x00  }, // Software reset (is it really necessary?)
    {0x11, 1 << 4}, // Push-Pull mode
};
static const _gpio_sequence gpio_deinit_seq[] = {};

#elif RG_I2C_GPIO_DRIVER == 2 // PCF9539

static const _gpio_port gpio_ports[] = {
    {0x00, 0x02, 0x06, -1}, // PORT 0
    {0x01, 0x03, 0x07, -1}, // PORT 1
};
static const _gpio_sequence gpio_init_seq[] = {};
static const _gpio_sequence gpio_deinit_seq[] = {};

#elif RG_I2C_GPIO_DRIVER == 3 // MCP23017

// Mappings when IOCON.BANK = 0 (which should be default on power-on)
static const _gpio_port gpio_ports[] = {
    {0x12, 0x14, 0x00, 0x0C}, // PORT A
    {0x13, 0x15, 0x01, 0x0D}, // PORT B
};
static const _gpio_sequence gpio_init_seq[] = {};
static const _gpio_sequence gpio_deinit_seq[] = {};

#elif RG_I2C_GPIO_DRIVER == 4 // PCF8575

static const _gpio_port gpio_ports[] = {
    {-1, -1, -1, -1}, // PORT 0
    {-1, -1, -1, -1}, // PORT 1
};
static const _gpio_sequence gpio_init_seq[] = {};
static const _gpio_sequence gpio_deinit_seq[] = {};
static rg_gpio_mode_t PCF857x_mode = -1;

#elif RG_I2C_GPIO_DRIVER == 5 // PCF8574

static const _gpio_port gpio_ports[] = {
    {-1, -1, -1, -1}, // PORT 0
};
static const _gpio_sequence gpio_init_seq[] = {};
static const _gpio_sequence gpio_deinit_seq[] = {};
static rg_gpio_mode_t PCF857x_mode = -1;

#else

#error "Unknown I2C GPIO Extender driver type!"

#endif

static const size_t gpio_ports_count = RG_COUNT(gpio_ports);
static uint8_t gpio_output_values[RG_COUNT(gpio_ports)];
static uint8_t gpio_address = RG_I2C_GPIO_ADDR;
static bool gpio_initialized = false;


bool rg_i2c_gpio_init(void)
{
    if (gpio_initialized)
        return true;

    if (!i2c_initialized && !rg_i2c_init())
        return false;

    // Configure extender-specific registers if needed (disable open-drain, interrupts, inversion, etc)
    for (size_t i = 0; (int)i < (int)RG_COUNT(gpio_init_seq); ++i)
    {
        if (!rg_i2c_write_byte(gpio_address, gpio_init_seq[i].reg, gpio_init_seq[i].value))
            goto fail;
    }

    // Set all pins as inputs and clear output latches
    for (size_t i = 0; i < gpio_ports_count; ++i)
    {
        if (!rg_i2c_gpio_configure_port(i, 0xFF, RG_GPIO_INPUT))
            goto fail;
        if (!rg_i2c_gpio_write_port(i, 0x00))
            goto fail;
    }

    RG_LOGI("GPIO Extender ready (driver:%d, addr:0x%02X).", RG_I2C_GPIO_DRIVER, gpio_address);
    gpio_initialized = true;
    return true;
fail:
    RG_LOGE("Failed to initialize extender (driver:%d, addr:0x%02X).", RG_I2C_GPIO_DRIVER, gpio_address);
    gpio_initialized = false;
    return false;
}

bool rg_i2c_gpio_deinit(void)
{
    if (gpio_initialized)
    {
        for (size_t i = 0; (int)i < (int)RG_COUNT(gpio_deinit_seq); ++i)
            rg_i2c_write_byte(gpio_address, gpio_deinit_seq[i].reg, gpio_deinit_seq[i].value);
        // Should we reset all pins to be high impedance?
        gpio_initialized = false;
    }
    return true;
}

static bool update_register(int reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value;
    return rg_i2c_read(gpio_address, reg, &value, 1) &&
           rg_i2c_write_byte(gpio_address, reg, (value & ~clear_mask) | set_mask);
}

bool rg_i2c_gpio_configure_port(int port, uint8_t mask, rg_gpio_mode_t mode)
{
    if (port < 0 || port >= gpio_ports_count)
        return false;
#if RG_I2C_GPIO_DRIVER == 4 || RG_I2C_GPIO_DRIVER == 5 // PCF8575/PCF8574
    uint32_t temp = 0xFFFFFFFF;
    if (mask != 0xFF && mode != PCF857x_mode && (mode == RG_GPIO_OUTPUT || PCF857x_mode == RG_GPIO_OUTPUT))
        RG_LOGW("PCF857x mode cannot be set by pin. (mask is 0x%02X, expected 0xFF)", mask);
    PCF857x_mode = mode;
    if (mode != RG_GPIO_OUTPUT)
        return rg_i2c_write(gpio_address, -1, &temp, gpio_ports_count)
            && rg_i2c_read(gpio_address, -1, &temp, gpio_ports_count);
    return rg_i2c_write(gpio_address, -1, &gpio_output_values, gpio_ports_count);
#else
    int direction_reg = gpio_ports[port].direction_reg;
    int pullup_reg = gpio_ports[port].pullup_reg;
    if (pullup_reg != -1 && !update_register(pullup_reg, mask, mode == RG_GPIO_INPUT_PULLUP ? mask : 0))
        return false;
    return update_register(direction_reg, mask, mode != RG_GPIO_OUTPUT ? mask : 0);
#endif
}

int rg_i2c_gpio_read_port(int port)
{
    if (port < 0 || port >= gpio_ports_count)
        return -1;
#if RG_I2C_GPIO_DRIVER == 4 || RG_I2C_GPIO_DRIVER == 5 // PCF8575/PCF8574
    uint8_t values[gpio_ports_count];
    return rg_i2c_read(gpio_address, -1, &values, gpio_ports_count) ? values[port] : -1;
#else
    return rg_i2c_read_byte(gpio_address, gpio_ports[port].input_reg);
#endif
}

bool rg_i2c_gpio_write_port(int port, uint8_t value)
{
    if (port < 0 || port >= gpio_ports_count)
        return false;
    gpio_output_values[port] = value;
#if RG_I2C_GPIO_DRIVER == 4 || RG_I2C_GPIO_DRIVER == 5 // PCF8575/PCF8574
    if (PCF857x_mode != RG_GPIO_OUTPUT)
        return true; // This is consistent with other extenders, where the output latch is updated even in input mode
    return rg_i2c_write(gpio_address, -1, &gpio_output_values, gpio_ports_count);
#else
    return rg_i2c_write_byte(gpio_address, gpio_ports[port].output_reg, value);
#endif
}

bool rg_i2c_gpio_set_direction(int pin, rg_gpio_mode_t mode)
{
    return rg_i2c_gpio_configure_port(pin >> 3, 1 << (pin & 7), mode);
}

int rg_i2c_gpio_get_level(int pin)
{
    return (rg_i2c_gpio_read_port(pin >> 3) >> (pin & 7)) & 1;
}

bool rg_i2c_gpio_set_level(int pin, int level)
{
    uint8_t port = (pin >> 3) % gpio_ports_count, mask = 1 << (pin & 7);
    uint8_t value = (gpio_output_values[port] & ~mask) | (level ? mask : 0);
    return rg_i2c_gpio_write_port(port, value);
}
#endif
