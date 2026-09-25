// USB Host 服务层
//
// 职责边界: 本模块只负责与"USB 是什么设备"无关的那部分 —— 安装 USB Host Library、
// 注册客户端、跑事件泵任务、发现设备、open/close、把拔出事件分发给认领了该设备的
// 类驱动。所有 class-specific 的逻辑(HID 报告描述符、boot protocol、MSC 的 SCSI
// 状态机等)都在类驱动里, 通过下面的三个回调接进来。
//
// 之所以要有这一层: drivers/input/usb_hid.c 最初把"主机生命周期"和"HID 键盘"两件事
// 写在一起。板卡将来可能接入其它 USB 设备(手柄、U 盘), 那些设备需要的正是同一套
// 主机生命周期, 而各自只需要自己的类逻辑。
//
// ── 【重要】本头文件包含 <usb/usb_host.h>, 因此绝不能进入 rg_system.h 的包含链 ──
// usb_host.h -> hal/gpio_types.h -> esp_bit_defs.h 会定义对象式宏 BIT0..BIT31, 而
// snes9x 的 cpuops.c 把 BIT8()/BIT16() 定义成【函数】(65C816 的 BIT 指令), gnuboy/
// nofrendo/smsplus/gwenesis 也各自定义了 BIT()。rg_system.h 会流入每一个核心的
// 每一个编译单元, 一旦这条链被打通就是全仓库编译失败(这个坑已经踩过一次)。
// 允许包含本文件的只有 drivers/ 下的 USB 相关 .c。rg_input.c 走的是
// drivers/input/usb_hid.h, 那个头文件只用纯 C 类型, 是安全的。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <usb/usb_host.h>

// 同时可注册的类驱动数量。目前只有 HID boot 键盘一个。
#define RG_USB_MAX_CLASSES  4
// 同时可被认领的设备数量。留了余量, 因为键盘和手柄是可以同时插着的。
#define RG_USB_MAX_DEVICES  4

typedef struct
{
    // 服务已经 usb_host_device_open() 打开设备并交给你检视。
    // 返回 true 表示认领 —— 此时你【必须】已经 claim 了你要用的接口, 因为服务之后
    // 只会调 detach(), 不会替你释放接口(而 usb_host_device_close() 要求接口已全部
    // 释放, 否则返回 ESP_ERR_INVALID_STATE)。
    // 返回 false 表示不是你的设备, 服务会继续问下一个类驱动, 全部拒绝后由服务 close。
    // 注意: 即使认领后立刻发现初始化失败(比如第一个控制传输提交不了), 也应该返回
    // true 让统一的 detach 路径去清理, 而不是在这里回滚 —— 接口已经 claim 了。
    bool (*probe)(uint8_t address, usb_device_handle_t dev, void *arg);

    // 每轮事件泵之后调用一次, 用来推进本类驱动的异步状态机。
    // 【为什么需要这个】USB 传输的完成回调是从 usb_host_client_handle_events() 里
    // 派发的, 也就是从服务的事件泵任务里。所以类驱动绝不能在自己的回调里阻塞等待
    // 另一个回调 —— 那是自己等自己, 必然死锁。链式的控制传输(例如 HID 的
    // SET_PROTOCOL -> SET_IDLE -> 开始收报告)只能靠这个 poll 一轮推进一步。
    // 返回 false 表示放弃该设备, 服务随后转入 detach 流程。
    bool (*poll)(void *arg);

    // 设备被拔出, 或 poll 返回了 false。负责释放接口、释放自己的 transfer 对象。
    // 返回 false 表示还没准备好(例如还有 URB 在途), 服务下一轮会再调一次;
    // 返回 true 之后服务才会 usb_host_device_close()。
    bool (*detach)(void *arg);
} rg_usb_class_t;

// 安装 USB Host Library、注册客户端、启动事件泵任务。幂等, 可被多个子系统调用。
// 非阻塞: 返回时设备很可能还没枚举完。
bool rg_usb_host_init(void);

// 注册一个类驱动。可在 rg_usb_host_init() 之前或之后调用; 之后调用会触发一次重新
// 扫描, 让新驱动有机会检视已经在位的设备。
bool rg_usb_host_add_class(const rg_usb_class_t *cls, void *arg);

// 客户端句柄。类驱动需要它来调 usb_host_interface_claim() / _release() 和
// usb_host_transfer_submit_control() —— 这三个 IDF API 都要求传客户端句柄, 而客户端
// 由服务独占注册(一个进程里注册多个客户端没有意义, 只会让事件分发变复杂)。
// rg_usb_host_init() 成功之前返回 NULL。
usb_host_client_handle_t rg_usb_host_client(void);
