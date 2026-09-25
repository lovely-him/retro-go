#include "rg_system.h"

// 整个文件仅在 target 定义 RG_USB_HOST_ENABLE 时才有内容。这样 drivers/usb 可以无条件
// 加入 COMPONENT_SRCDIRS: 其它 target 得到的只是一个空翻译单元, 既不会引用
// components/usb, 也不会因为该组件在 IDF 4.x 上行为不同而构建失败。
#ifdef RG_USB_HOST_ENABLE

#include <string.h>

#include <usb/usb_host.h>

#include "usb_host_svc.h"

// 栈要给到 5KB: rg_system_vlog() 自带 char buffer[300], 加 newlib vsnprintf 的内部
// 开销, 一条 RG_LOGI 就吃掉约 800 字节, 再叠加 usb_host 枚举路径的调用深度。
#define USB_TASK_STACK_SIZE     5120
#define USB_TASK_PRIORITY       RG_TASK_PRIORITY_4
#define USB_PENDING_MAX         4

typedef struct
{
    const rg_usb_class_t *cls;
    void *arg;
    uint8_t address;
    usb_device_handle_t dev;    // NULL 表示本槽位空闲
    bool detaching;
} usb_slot_t;

static usb_host_client_handle_t usb_client;
static bool usb_task_running;

static const rg_usb_class_t *classes[RG_USB_MAX_CLASSES];
static void *class_args[RG_USB_MAX_CLASSES];
static size_t classes_count;

static usb_slot_t slots[RG_USB_MAX_DEVICES];

// 待探测地址队列。用队列而不是单个变量: 一个 hub 下可以挂多个设备, 它们可能在同一轮
// 事件泵里先后枚举完, 单个变量会被后一个覆盖从而丢掉前一个。
static uint8_t pending[USB_PENDING_MAX];
static size_t pending_head;
static size_t pending_count;
static bool rescan_needed;

// ---------------------------------------------------------------------------
// 设备发现
// ---------------------------------------------------------------------------

static void pending_push(uint8_t address)
{
    if (address == 0)
        return;

    for (size_t i = 0; i < pending_count; ++i)
    {
        if (pending[(pending_head + i) % USB_PENDING_MAX] == address)
            return;
    }
    for (size_t i = 0; i < RG_USB_MAX_DEVICES; ++i)
    {
        if (slots[i].dev && slots[i].address == address)
            return; // 已被某个类驱动认领
    }

    if (pending_count >= USB_PENDING_MAX)
    {
        RG_LOGW("usb-host: pending queue full, dropping addr %d", address);
        return;
    }
    pending[(pending_head + pending_count) % USB_PENDING_MAX] = address;
    pending_count++;
}

// 用 usb_host_device_addr_list_fill() 而不是直接消费 NEW_DEV 事件里的地址:
// 客户端注册之前就已枚举完成的设备不会补发该事件, 而 addr_list_fill 走的是 usbh 的
// devs_idle_tailq 全量列表(与父设备是根端口还是外接 hub 无关)。这样冷启动和热插拔
// 共用同一条逻辑。
//
// 代价: 被所有类驱动拒绝过的设备(例如 hub 本身)不会进入 slots, 于是下一次 rescan
// 会把它重新入队再探一次。这只发生在有新设备插入时(每次热插拔多一次 open/close
// 加一条日志), 不是每轮循环, 所以刻意没有为此维护"已拒绝地址表"。
static void usb_rescan(void)
{
    uint8_t addrs[USB_PENDING_MAX];
    int count = 0;

    rescan_needed = false;

    if (usb_host_device_addr_list_fill(USB_PENDING_MAX, addrs, &count) != ESP_OK)
        return;
    for (int i = 0; i < count; ++i)
        pending_push(addrs[i]);
}

static void probe_next(void)
{
    usb_slot_t *slot = NULL;
    for (size_t i = 0; i < RG_USB_MAX_DEVICES; ++i)
    {
        if (!slots[i].dev)
        {
            slot = &slots[i];
            break;
        }
    }
    if (!slot || pending_count == 0)
        return; // 没有空槽位时地址留在队列里, 不出队

    const uint8_t address = pending[pending_head];
    pending_head = (pending_head + 1) % USB_PENDING_MAX;
    pending_count--;

    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(usb_client, address, &dev) != ESP_OK)
        return; // 设备已消失

    for (size_t i = 0; i < classes_count; ++i)
    {
        if (classes[i]->probe(address, dev, class_args[i]))
        {
            slot->cls = classes[i];
            slot->arg = class_args[i];
            slot->address = address;
            slot->dev = dev;
            slot->detaching = false;
            RG_LOGI("usb-host: addr %d claimed", address);
            return;
        }
    }

    usb_host_device_close(usb_client, dev);
}

static void device_release(usb_slot_t *slot)
{
    // 类驱动的 detach() 返回 true 时才走到这里, 那时接口已经释放完毕 —— 这是必须的,
    // usb_host_device_close() 会检查该客户端是否还持有本设备的接口, 未释放则返回
    // ESP_ERR_INVALID_STATE 且不关闭(usb_host.c:1039)。
    usb_host_device_close(usb_client, slot->dev);
    RG_LOGI("usb-host: addr %d released", slot->address);
    memset(slot, 0, sizeof(*slot));
}

// ---------------------------------------------------------------------------
// 事件泵
// ---------------------------------------------------------------------------

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        rescan_needed = true;
    }
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
    {
        for (size_t i = 0; i < RG_USB_MAX_DEVICES; ++i)
        {
            if (slots[i].dev == msg->dev_gone.dev_hdl)
                slots[i].detaching = true;
        }
    }
}

static void usb_task(void *arg)
{
    while (usb_task_running)
    {
        // 一次调用即可: 该函数内部会循环到事件队列为空, 且 timeout_ticks 为 0 时恒
        // 返回 ESP_OK(usb_host.c:666), 所以绝不能写成 while(... == ESP_OK) 去抽干。
        uint32_t lib_flags = 0;
        usb_host_lib_handle_events(0, &lib_flags);

        // 阻塞上限 10ms: 既是本任务的节流点, 也决定热插拔的响应延迟
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));

        for (size_t i = 0; i < RG_USB_MAX_DEVICES; ++i)
        {
            usb_slot_t *slot = &slots[i];
            if (!slot->dev)
                continue;

            if (slot->detaching)
            {
                // detach 可能要等多轮: URB 在途时不能释放 transfer 对象。设备拔出后
                // IDF 会以 NO_DEVICE 完成所有在途 URB(hcd_dwc.c:1820), 回调从本任务
                // 的 usb_host_client_handle_events() 派发, 所以下一轮必然能等到。
                if (slot->cls->detach(slot->arg))
                    device_release(slot);
            }
            else if (!slot->cls->poll(slot->arg))
            {
                slot->detaching = true;
            }
        }

        if (rescan_needed)
            usb_rescan();
        if (pending_count)
            probe_next();
    }

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

bool rg_usb_host_init(void)
{
    if (usb_client)
        return true;

    const usb_host_config_t host_config = {
        // P4 上默认外设就是 USB2.0 OTG High-Speed, 正是板上 Type-A 口所接的控制器,
        // 所以 peripheral_map 留 0。skip_phy_setup 为 false 让库自己配置内部 PHY。
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK)
    {
        RG_LOGE("usb-host: usb_host_install failed (%s)", esp_err_to_name(err));
        return false;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    err = usb_host_client_register(&client_config, &usb_client);
    if (err != ESP_OK)
    {
        RG_LOGE("usb-host: client register failed (%s)", esp_err_to_name(err));
        usb_client = NULL;
        return false;
    }

    usb_task_running = true;
    rg_task_create("rg_usb", usb_task, NULL, USB_TASK_STACK_SIZE, USB_TASK_PRIORITY, -1);

    // 注册之前就已枚举完成的设备不会补发 NEW_DEV, 所以先主动扫一次
    rescan_needed = true;

    // 刻意不打印类驱动数量: 本函数在 rg_usb_host_add_class() 之前被调用, 那时计数
    // 必然还是 0, 打出来会误导排查。各驱动自己会打注册日志。
    RG_LOGI("usb-host: installed");
    return true;
}

bool rg_usb_host_add_class(const rg_usb_class_t *cls, void *arg)
{
    if (!cls || !cls->probe || !cls->poll || !cls->detach)
    {
        RG_LOGE("usb-host: class driver is missing a callback");
        return false;
    }
    if (classes_count >= RG_USB_MAX_CLASSES)
    {
        RG_LOGE("usb-host: class table full (max %d)", RG_USB_MAX_CLASSES);
        return false;
    }

    classes[classes_count] = cls;
    class_args[classes_count] = arg;
    classes_count++;

    // 万一在设备已枚举之后才注册, 让新驱动仍有机会检视在位设备
    rescan_needed = true;
    return true;
}

usb_host_client_handle_t rg_usb_host_client(void)
{
    return usb_client;
}

#endif // RG_USB_HOST_ENABLE
