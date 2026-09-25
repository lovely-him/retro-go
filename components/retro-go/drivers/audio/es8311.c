// ES8311 codec 音频驱动
//
// 整个文件仅在 target 定义 RG_AUDIO_USE_ES8311 时参与编译, 与 drivers/audio/i2s.c 用
// RG_AUDIO_USE_INT_DAC / RG_AUDIO_USE_EXT_DAC 门控是同一套做法。
//
// 为什么不复用 i2s.c: 它服务的是"哑 DAC"(收到 I2S 数据就直接出声, 无需配置), 而
// ES8311 是带内部时钟树的 codec, 必须先经 I2C 写几十个寄存器才能工作; 而且它把
// mck_io_num 硬设为 GPIO_NUM_NC, 本板的 ES8311 必须要 MCLK。另外它 include 的
// <driver/i2s.h> 是 IDF 5.5 已移除的 legacy 驱动。所以这是新驱动而不是改 i2s.c。
//
// ── 关键设计: MCLK 恒为 256 x fs, 于是 codec 的分频配置是常量 ──────────────
// retro-go 的采样率不是固定几个值。基准因 app 而异(retro-core/fmsx 32000、
// PCE/prboom 22050、gwenesis 26633), 再叠加快进 rg_system_set_app_speed() 的
// 0.5~2.5 倍(rg_gui.c 的 speedup_update_cb 以 0.5 步进), 所以实际会出现 11025、
// 13317、26633、39950、53266、80000 这类值。
//
// 厂商的 ES8311 驱动(espressif/es8311 与 esp_codec_dev 都是)按 (mclk, fs) 精确查表,
// 表里没有的速率会失败 —— 而 esp_codec_dev 的 es8311.c:649 调用 es8311_config_sample()
// 时【忽略了返回值】, 结果是静默沿用上一次时钟配置, 音调错误且上层毫无察觉。上面
// 那些速率一个都不在表里。
//
// 本驱动不需要查表。IDF 的 I2S_STD_CLK_DEFAULT_CONFIG() 默认 mclk_multiple = 256,
// 也就是 MCLK 恒等于 256 x fs; 而 ES8311 的 REG07/REG08 正是"MCLK 分频成 LRCK"的
// 分频器, 写 0x00FF 就是除以 256。既然比值恒定, 所有分频寄存器就与 fs 无关 ——
// 这一点在厂商表里也能逐行验证: mclk/fs = 256 的那些行(2048000/8000、4096000/16000、
// 5644800/22050、6144000/24000、8192000/32000、11289600/44100、12288000/48000、
// 16384000/64000)的 pre_div/pre_multi/adc_div/dac_div/fs_mode/lrck/bclk_div/osr
// 全部完全相同。
//
// 于是: codec 只在 init 时配置一次, 之后 set_sample_rate 只重配 I2S, 完全不碰 codec。
// 任意采样率都成立, 包括厂商表覆盖不到的那些。
//
// ── 音量放在硬件 ────────────────────────────────────────────────────────
// i2s.c 的做法是在 submit() 里对每个样本做浮点乘法, 还要先拷进一个 720 字节的栈缓冲。
// ES8311 有 DAC 音量寄存器(REG32), 用它之后 submit() 可以把调用方的 buffer 原样交给
// i2s_channel_write(), 省掉整趟拷贝和逐样本乘法 —— 这条路径每秒要跑 32000 次。
// 副作用是 set_volume() 不再与 DMA 路径共享任何状态, 而 rg_audio_set_volume() 是不持
// 设备锁调用的(rg_audio.c:192), 所以这恰好也是并发安全所要求的。

#include "rg_system.h"
#include "rg_audio.h"

#ifdef RG_AUDIO_USE_ES8311

#ifndef ESP_PLATFORM
#error "ES8311 support can only be built inside esp-idf!"
#endif

#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>

// ---- ES8311 寄存器地址 ----
#define ES8311_REG_RESET        0x00    // 复位 / CSM 上下电 / 主从模式(BIT6)
#define ES8311_REG_CLK_MGR1     0x01    // MCLK 源选择, 各时钟使能
#define ES8311_REG_CLK_MGR2     0x02    // pre_div(bits7:5) 与 pre_multi(bits4:3)
#define ES8311_REG_CLK_MGR3     0x03    // fs_mode(bit6) 与 adc_osr
#define ES8311_REG_CLK_MGR4     0x04    // dac_osr
#define ES8311_REG_CLK_MGR5     0x05    // adc_div(bits7:4) 与 dac_div(bits3:0)
#define ES8311_REG_CLK_MGR6     0x06    // SCLK 反相(BIT5) 与 bclk_div(bits4:0)
#define ES8311_REG_CLK_MGR7     0x07    // LRCK 分频高字节(bits5:0), bits7:6 为三态控制
#define ES8311_REG_CLK_MGR8     0x08    // LRCK 分频低字节
#define ES8311_REG_SDPIN        0x09    // DAC 侧串行口(即 P4 -> codec 的放音通路)
#define ES8311_REG_SDPOUT       0x0A    // ADC 侧串行口
#define ES8311_REG_SYSTEM0D     0x0D    // 模拟电路上电
#define ES8311_REG_SYSTEM0E     0x0E    // 模拟 PGA 与 ADC 调制器
#define ES8311_REG_SYSTEM12     0x12    // DAC 上电
#define ES8311_REG_SYSTEM13     0x13    // 耳机驱动输出使能
#define ES8311_REG_SYSTEM14     0x14    // 麦克风类型选择与模拟 PGA 增益
#define ES8311_REG_ADC17        0x17    // ADC 音量
#define ES8311_REG_ADC1C        0x1C    // ADC 均衡器 / 数字域直流消除
#define ES8311_REG_DAC31        0x31    // DAC 静音(bits6:5)
#define ES8311_REG_DAC32        0x32    // DAC 音量
#define ES8311_REG_DAC37        0x37    // DAC 均衡器旁路 / ramprate

// REG31 bits6:5 = 静音控制。厂商 esp_codec_dev 用 "&= 0x9F" 解除静音, 即两位都清零。
#define ES8311_DAC_MUTE_BITS    0x60
#define ES8311_DAC_UNMUTE_KEEP  0x9F

// SDP 位宽字段在 bits3:2, 16-bit 对应 3
#define ES8311_SDP_16BIT        (3 << 2)

// 功放使能脚的有效电平。沿用 i2s.c 的约定: 定义了 _INVERT 表示"静音 = 高电平"。
#ifdef RG_GPIO_SND_AMP_ENABLE
#ifdef RG_GPIO_SND_AMP_ENABLE_INVERT
#define AMP_LEVEL_ON  0
#define AMP_LEVEL_OFF 1
#else
#define AMP_LEVEL_ON  1
#define AMP_LEVEL_OFF 0
#endif
#endif

#ifndef RG_AUDIO_I2S_PORT
#define RG_AUDIO_I2S_PORT I2S_NUM_0
#endif

// i2s_channel_write 的阻塞上限。submit() 靠这个阻塞来给模拟器提供实时节奏,
// 与 i2s.c 用的 1000ms 一致。
#define I2S_WRITE_TIMEOUT_MS 1000

static struct
{
    const char *last_error;
    int volume;
    bool codec_ready;
} state;

static i2s_chan_handle_t tx_chan;

// ---------------------------------------------------------------------------
// 功放与 I2C 底层
// ---------------------------------------------------------------------------

static void amp_set(bool on)
{
#ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_set_level(RG_GPIO_SND_AMP_ENABLE, on ? AMP_LEVEL_ON : AMP_LEVEL_OFF);
#else
    (void)on;
#endif
}

static bool codec_write(uint8_t reg, uint8_t value)
{
    if (!rg_i2c_write_byte(RG_AUDIO_CODEC_I2C_ADDR, reg, value))
    {
        state.last_error = "ES8311 I2C write failed";
        RG_LOGE("es8311: write reg 0x%02X = 0x%02X failed", reg, value);
        return false;
    }
    return true;
}

static int codec_read(uint8_t reg)
{
    const int value = rg_i2c_read_byte(RG_AUDIO_CODEC_I2C_ADDR, reg);
    if (value < 0)
    {
        state.last_error = "ES8311 I2C read failed";
        RG_LOGE("es8311: read reg 0x%02X failed", reg);
    }
    return value;
}

// 读-改-写。keep_mask 指明要【保留】的位, 与厂商驱动的写法一致, 便于逐行对照。
static bool codec_update(uint8_t reg, uint8_t keep_mask, uint8_t set_bits)
{
    const int value = codec_read(reg);
    if (value < 0)
        return false;
    return codec_write(reg, (uint8_t)((value & keep_mask) | set_bits));
}

// ---------------------------------------------------------------------------
// codec 配置。寄存器值取自厂商在本板上已验证的序列(espressif/es8311 一脉,
// 板卡仓库 examples/arduino/examples/Audio_Playback/es8311.c), 逐行可对照。
// ---------------------------------------------------------------------------

static bool es8311_configure(void)
{
    // 复位, 然后给 CSM 上电
    if (!codec_write(ES8311_REG_RESET, 0x1F))
        return false;
    rg_task_delay(20);
    if (!codec_write(ES8311_REG_RESET, 0x00))
        return false;
    if (!codec_write(ES8311_REG_RESET, 0x80))
        return false;

    // 0x3F = 使能全部内部时钟; BIT7 清零表示 MCLK 取自 MCLK 引脚(而非 BCLK),
    // BIT6 清零表示 MCLK 不反相。
    if (!codec_write(ES8311_REG_CLK_MGR1, 0x3F))
        return false;
    // SCLK 不反相: 清 BIT5, 其余位保留
    if (!codec_update(ES8311_REG_CLK_MGR6, 0xDF, 0x00))
        return false;

    // 分频。MCLK = 256 x fs 恒定, 所以以下全是常量, 与当前采样率无关(见文件头)。
    if (!codec_update(ES8311_REG_CLK_MGR2, 0x07, 0x00))
        return false; // pre_div=1, pre_multi=1x
    if (!codec_write(ES8311_REG_CLK_MGR3, 0x10))
        return false; // 单速模式 + adc_osr
    if (!codec_write(ES8311_REG_CLK_MGR4, 0x10))
        return false; // dac_osr
    if (!codec_write(ES8311_REG_CLK_MGR5, 0x00))
        return false; // adc_div=1, dac_div=1
    if (!codec_update(ES8311_REG_CLK_MGR6, 0xE0, 0x03))
        return false; // bclk_div=4, 寄存器写 4-1
    if (!codec_update(ES8311_REG_CLK_MGR7, 0xC0, 0x00))
        return false; // LRCK 分频高字节
    if (!codec_write(ES8311_REG_CLK_MGR8, 0xFF))
        return false; // LRCK = MCLK / 0x00FF+1 = MCLK / 256

    // 串行口: 从机模式(REG00 的 BIT6 清零) + I2S Philips + 16-bit
    if (!codec_update(ES8311_REG_RESET, 0xBF, 0x00))
        return false;
    if (!codec_write(ES8311_REG_SDPIN, ES8311_SDP_16BIT))
        return false;
    if (!codec_write(ES8311_REG_SDPOUT, ES8311_SDP_16BIT))
        return false;

    // 模拟电路上电
    if (!codec_write(ES8311_REG_SYSTEM0D, 0x01))
        return false;
    if (!codec_write(ES8311_REG_SYSTEM0E, 0x02))
        return false;
    if (!codec_write(ES8311_REG_SYSTEM12, 0x00))
        return false; // DAC 上电
    if (!codec_write(ES8311_REG_SYSTEM13, 0x10))
        return false; // 耳机驱动输出
    if (!codec_write(ES8311_REG_ADC1C, 0x6A))
        return false; // ADC 均衡器旁路 + 数字域直流消除
    if (!codec_write(ES8311_REG_DAC37, 0x08))
        return false; // DAC 均衡器旁路

    // ADC/麦克风通路。本驱动只放音不录音, 但仍按厂商已验证的序列写完: init 只执行
    // 一次, 省这两条 I2C 写没有意义, 而偏离已验证序列的调试代价大得多。
    if (!codec_write(ES8311_REG_ADC17, 0xC8))
        return false;
    if (!codec_write(ES8311_REG_SYSTEM14, 0x1A))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------

static bool i2s_setup(int sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(RG_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    // 数据耗尽时自动清零 DMA 缓冲。不开的话暂停或退出时会反复重放最后一段数据,
    // 表现为持续的嗡嗡声。IDF 5.5 已把旧的 auto_clear 拆成 after_cb / before_cb,
    // after_cb 等价于旧语义(板卡 BSP 用的旧字段名在 5.5 编不过)。
    chan_cfg.auto_clear_after_cb = true;

    // 只建 TX: 本驱动不录音, 省下 RX 通道和它那份 DMA 缓冲(内部 RAM 是紧资源)。
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK)
    {
        state.last_error = "i2s_new_channel failed";
        RG_LOGE("es8311: i2s_new_channel failed (%s)", esp_err_to_name(err));
        tx_chan = NULL;
        return false;
    }

    const i2s_std_config_t std_cfg = {
        // 默认即 mclk_multiple = I2S_MCLK_MULTIPLE_256, bclk_div = 8
        // => MCLK = 256fs, BCLK = MCLK/8 = 32fs, 正好是 16-bit 立体声每帧所需。
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = RG_GPIO_SND_I2S_MCLK,
            .bclk = RG_GPIO_SND_I2S_BCK,
            .ws = RG_GPIO_SND_I2S_WS,
            .dout = RG_GPIO_SND_I2S_DATA,
            .din = I2S_GPIO_UNUSED,
        },
    };

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err == ESP_OK)
        err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK)
    {
        state.last_error = "i2s std mode init failed";
        RG_LOGE("es8311: I2S init failed (%s)", esp_err_to_name(err));
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// rg_audio_driver_t 实现
// ---------------------------------------------------------------------------

static bool driver_init(int device, int sample_rate)
{
    state.last_error = NULL;
    state.codec_ready = false;

#ifdef RG_GPIO_SND_AMP_ENABLE
    // 功放先关, 等 codec 配好再开, 避免上电爆音(厂商 esp_codec_dev 的时序)。
    // 先设电平再设方向, 与 i2s.c 一致, 免得方向切到输出的瞬间电平未定。
    gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
    gpio_set_level(RG_GPIO_SND_AMP_ENABLE, AMP_LEVEL_OFF);
    gpio_set_direction(RG_GPIO_SND_AMP_ENABLE, GPIO_MODE_OUTPUT);
#endif

    // I2C 总线归 rg_i2c.c 所有(与背光控制器 0x45、触摸 GT911 共用同一条), 这里只
    // 登记自己的速率。rg_i2c_init() 幂等, 显示驱动若已先建好总线则直接复用。
    rg_i2c_set_speed(RG_AUDIO_CODEC_I2C_ADDR, RG_AUDIO_CODEC_I2C_SPEED);
    if (!rg_i2c_init())
    {
        state.last_error = "I2C bus init failed";
        RG_LOGE("es8311: I2C bus init failed");
        return false;
    }

    // ES8311 是从机, 先让 I2S 把 MCLK/BCLK/LRCK 输出起来, 再配置 codec。
    if (!i2s_setup(sample_rate))
        return false;

    if (!es8311_configure())
    {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return false;
    }

    state.codec_ready = true;

    // 厂商顺序: 先开功放, 再解除 DAC 静音
    amp_set(true);
    codec_update(ES8311_REG_DAC31, ES8311_DAC_UNMUTE_KEEP, 0x00);

    RG_LOGI("es8311: ready, %d Hz (MCLK %d Hz), PA %s", sample_rate, sample_rate * 256,
#ifdef RG_GPIO_SND_AMP_ENABLE
            "on GPIO"
#else
            "none"
#endif
    );
    return true;
}

static bool driver_deinit(void)
{
    if (state.codec_ready)
        codec_update(ES8311_REG_DAC31, 0xFF, ES8311_DAC_MUTE_BITS);
    state.codec_ready = false;

    amp_set(false);

    if (tx_chan)
    {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    if (!tx_chan)
        return false;

    size_t written = 0;
    // 音量在 codec 硬件里, 所以这里不碰样本数据, 调用方的 buffer 原样提交。
    // rg_audio_frame_t 是 {int16_t left, right} 交错立体声, 与 I2S Philips 立体声槽位
    // 布局一致; 且两边都是本机字节序(与显示那条链刻意用大端不同), 无需交换。
    // 阻塞在这里是有意的: 它就是模拟器的实时节奏来源。
    const esp_err_t err =
        i2s_channel_write(tx_chan, frames, count * sizeof(rg_audio_frame_t), &written, I2S_WRITE_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        RG_LOGW("es8311: I2S write failed (%s), %u/%u bytes", esp_err_to_name(err), (unsigned)written,
                (unsigned)(count * sizeof(rg_audio_frame_t)));
        return false;
    }
    return true;
}

static bool driver_set_mute(bool mute)
{
    if (state.codec_ready)
        codec_update(ES8311_REG_DAC31, mute ? 0xFF : ES8311_DAC_UNMUTE_KEEP, mute ? ES8311_DAC_MUTE_BITS : 0x00);

    // 静音时一并关掉功放, 否则会把底噪放大出来
    amp_set(!mute);
    return true;
}

static bool driver_set_volume(int percent)
{
    state.volume = RG_MIN(RG_MAX(percent, 0), 100);
    if (!state.codec_ready)
        return true;

    // REG32 是 0.5dB/step 的 DAC 音量, 0x00 = -95.5dB, 0xBF 约 0dB。
    // 这里的映射与板卡自带例程一致, 代价是 0dB 落在约 75%, 再往上是正增益
    // (100% 约 +32dB), 高档位有削顶风险。先按厂商已验证的曲线走; 若实测高档位失真,
    // 改成 state.volume * 191 / 100 把上限钳在 0dB。
    const int reg = state.volume == 0 ? 0 : (state.volume * 256 / 100) - 1;
    return codec_write(ES8311_REG_DAC32, (uint8_t)reg);
}

static bool driver_set_sample_rate(int sample_rate)
{
    if (!tx_chan)
        return false;

    // i2s_channel_reconfig_std_clock() 要求通道处于 READY(已初始化但未启动), 所以
    // 必须先 disable。调用方 rg_audio_set_sample_rate() 持有设备锁(rg_audio.c:235),
    // 与 submit() 的 ACQUIRE_DEVICE(0) 互斥, 因此这里不会踩到在途的 write。
    // codec 完全不用碰: MCLK 始终是 256 x fs, 分频比不变(见文件头)。
    const i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    i2s_channel_disable(tx_chan);
    const esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    i2s_channel_enable(tx_chan);

    if (err != ESP_OK)
    {
        state.last_error = "sample rate reconfig failed";
        RG_LOGE("es8311: sample rate %d failed (%s)", sample_rate, esp_err_to_name(err));
        return false;
    }
    RG_LOGI("es8311: sample rate %d Hz (MCLK %d Hz)", sample_rate, sample_rate * 256);
    return true;
}

static const char *driver_get_error(void)
{
    return state.last_error;
}

const rg_audio_driver_t rg_audio_driver_es8311 = {
    .name = "es8311",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
    .set_sample_rate = driver_set_sample_rate,
    .get_error = driver_get_error,
};

#endif // RG_AUDIO_USE_ES8311
