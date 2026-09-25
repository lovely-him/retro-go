#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// 本文件经 rg_system.h 流入【每一个核心的每一个编译单元】, 所以这里绝不能出现任何
// IDF 驱动头文件: driver/i2c_master.h -> hal/gpio_types.h -> esp_bit_defs.h 会定义
// 对象式宏 BIT0..BIT31, 而 snes9x 的 cpuops.c 把 BIT8()/BIT16() 定义成函数(65C816
// 的 BIT 指令), gnuboy/nofrendo/smsplus/gwenesis 也各自定义了 BIT()。句柄一律用
// 纯 C 类型传递。

bool rg_i2c_init(void);
bool rg_i2c_deinit(void);
bool rg_i2c_read(uint8_t addr, int reg, void *read_data, size_t read_len);
bool rg_i2c_write(uint8_t addr, int reg, const void *write_data, size_t write_len);
int rg_i2c_read_byte(uint8_t addr, uint8_t reg);
bool rg_i2c_write_byte(uint8_t addr, uint8_t reg, uint8_t value);

// 登记某个从机地址的 SCL 速率, 必须在首次访问该地址之前调用。
// 只有定义了 RG_I2C_MASTER_DRIVER(新版 i2c_master API)才真正生效 —— 那条路径下速率
// 是每设备设置的, 所以同一条总线上不同器件可以各跑各的速率。未登记的地址用
// RG_I2C_SPEED。legacy 驱动只有总线级速率, 此时本函数返回 false 并打一条警告。
bool rg_i2c_set_speed(uint8_t addr, uint32_t speed_hz);

// 只发一次地址握手, 判断该地址上有没有器件 ACK。不占用从机表槽位, 也不创建设备
// 句柄, 所以可以安全地用来在多个候选地址里挑出真实存在的那个(如 GT911 的
// 0x5D/0x14)。探测不到时不会产生任何错误日志(IDF 内部置了 bypass_nack_log)。
// 仅在 RG_I2C_MASTER_DRIVER 下实现, 否则返回 false 并打一条警告。
// 注意: i2c_master_probe() 把 SCL 固定在 100kHz(i2c_master.c:1386), 与
// rg_i2c_set_speed() 登记的速率无关。这不构成限制 —— I2C 的速率是上限而非要求,
// Fast-mode 器件在 Standard-mode 下必然应答。
bool rg_i2c_probe(uint8_t addr);

// 16-bit 寄存器地址版本(GT911 触摸控制器需要: 地址 16 位大端, 数据小端)。
// 上面的 rg_i2c_read/write 的 reg 参数只发一个字节, 覆盖不到。
// 仅在 RG_I2C_MASTER_DRIVER 下实现, 否则返回 false 并打一条错误。
bool rg_i2c_read_reg16(uint8_t addr, uint16_t reg, void *read_data, size_t read_len);
bool rg_i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t value);


// GPIO extender
typedef enum
{
    RG_GPIO_OUTPUT,
    RG_GPIO_INPUT,
    RG_GPIO_INPUT_PULLUP,
} rg_gpio_mode_t;

bool rg_i2c_gpio_init(void);
bool rg_i2c_gpio_deinit(void);
bool rg_i2c_gpio_configure_port(int port, uint8_t mask, rg_gpio_mode_t mode);
int rg_i2c_gpio_read_port(int port);
bool rg_i2c_gpio_write_port(int port, uint8_t value);
// For the following functions `pin` is calculated as such: (port_num * 8) + port_pin_num
bool rg_i2c_gpio_set_direction(int pin, rg_gpio_mode_t mode);
int rg_i2c_gpio_get_level(int pin);
bool rg_i2c_gpio_set_level(int pin, int level);
