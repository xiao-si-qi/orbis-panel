/**
 * @file voice_audio.c
 * @brief I2S 麦克风录音 + PCM5100A 播放（阶段④音频底层）。
 *
 * 说明：
 *   - 麦克风是数字 I2S 麦（WS/SCK/SD），单声道 16bit；
 *   - 喇叭是 PCM5100A（纯 DAC，无 I2C 控制），只有 BCLK/WS/DOUT + 一个静音脚；
 *   - 采样率保持调用方指定（录音 16k、播放 24k，与小智协议一致）。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "voice_audio.h"

static const char *TAG = "voice_audio";

#define MIC_WS_GPIO    45
#define MIC_SCK_GPIO   42
#define MIC_SD_GPIO    46

#define SPK_BCLK_GPIO  18
#define SPK_WS_GPIO    16
#define SPK_DOUT_GPIO  17
#define SPK_MUTE_GPIO  48   /* 低电平静音 */

static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static bool s_ready;

static esp_err_t mic_init(int sample_rate)
{
    if (s_rx != NULL) {
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风通道创建失败: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD_GPIO,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "麦克风就绪（WS=%d SCK=%d SD=%d, %d Hz）", MIC_WS_GPIO, MIC_SCK_GPIO, MIC_SD_GPIO, sample_rate);
    }
    return err;
}

static esp_err_t spk_init(int sample_rate)
{
    if (s_tx != NULL) {
        return ESP_OK;
    }
    gpio_config_t mute = {
        .pin_bit_mask = 1ULL << SPK_MUTE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&mute);
    gpio_set_level(SPK_MUTE_GPIO, 0);   /* 先静音，播放时打开 */

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "喇叭通道创建失败: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "喇叭初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "喇叭就绪（BCLK=%d WS=%d DOUT=%d, %d Hz）", SPK_BCLK_GPIO, SPK_WS_GPIO, SPK_DOUT_GPIO, sample_rate);
    }
    return err;
}

int voice_audio_init(void)
{
    if (s_ready) {
        return 0;
    }
    if (mic_init(16000) != ESP_OK) {
        return -1;
    }
    if (spk_init(24000) != ESP_OK) {
        return -1;
    }
    s_ready = true;
    return 0;
}

int voice_audio_ready(void)
{
    return s_ready ? 1 : 0;
}

void voice_audio_mute(int on)
{
    gpio_set_level(SPK_MUTE_GPIO, on ? 0 : 1);
}

int voice_audio_record(int16_t *buf, int max_samples, int sample_rate, int ms)
{
    if (!s_ready || buf == NULL || max_samples <= 0) {
        return -1;
    }
    /* 采样率变了要重建通道 */
    if (sample_rate != 16000) {
        if (s_rx != NULL) {
            i2s_channel_disable(s_rx);
            i2s_del_channel(s_rx);
            s_rx = NULL;
        }
        if (mic_init(sample_rate) != ESP_OK) {
            return -1;
        }
    }

    int want = (int)((int64_t)sample_rate * ms / 1000);
    if (want > max_samples) {
        want = max_samples;
    }

    size_t got_bytes = 0;
    const size_t want_bytes = (size_t)want * sizeof(int16_t);
    const esp_err_t err = i2s_channel_read(s_rx, buf, want_bytes, &got_bytes, pdMS_TO_TICKS(ms + 2000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "录音失败: %s", esp_err_to_name(err));
        return -1;
    }
    return (int)(got_bytes / sizeof(int16_t));
}

int voice_audio_play(const int16_t *buf, int samples, int sample_rate)
{
    if (!s_ready || buf == NULL || samples <= 0) {
        return -1;
    }
    /* 播放采样率按需重建（16k/24k 两种） */
    static int s_tx_rate;
    if (s_tx_rate != sample_rate) {
        if (s_tx != NULL) {
            i2s_channel_disable(s_tx);
            i2s_del_channel(s_tx);
            s_tx = NULL;
        }
        if (spk_init(sample_rate) != ESP_OK) {
            return -1;
        }
        s_tx_rate = sample_rate;
    }

    voice_audio_mute(0);
    size_t written = 0;
    const size_t want = (size_t)samples * sizeof(int16_t);
    const esp_err_t err = i2s_channel_write(s_tx, buf, want, &written, pdMS_TO_TICKS(samples * 1000 / sample_rate + 3000));
    voice_audio_mute(1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "播放失败: %s", esp_err_to_name(err));
        return -1;
    }
    return (int)(written / sizeof(int16_t));
}

int voice_audio_selftest(int *peak, int *rms)
{
    const int seconds_ms = 1500;
    const int rate = 16000;
    const int max_samples = rate * seconds_ms / 1000;
    int16_t *buf = heap_caps_malloc((size_t)max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "自检缓冲分配失败");
        return -1;
    }
    if (voice_audio_init() != 0) {
        free(buf);
        return -1;
    }

    ESP_LOGI(TAG, "自检：录音 %d ms …", seconds_ms);
    const int n = voice_audio_record(buf, max_samples, rate, seconds_ms);
    if (n <= 0) {
        free(buf);
        return -1;
    }

    int32_t pk = 0;
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        const int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > pk) {
            pk = v;
        }
        sum += (int64_t)buf[i] * buf[i];
    }
    const int r = (int)(sum / n);
    if (peak) {
        *peak = (int)pk;
    }
    if (rms) {
        *rms = r;
    }
    ESP_LOGI(TAG, "自检结果：%d 采样，峰值 %d，均方 %d %s", n, (int)pk, r,
             pk < 200 ? "（几乎没有声音，检查麦克风接线）" : "");

    ESP_LOGI(TAG, "自检：回放刚才的录音 …");
    voice_audio_play(buf, n, rate);
    free(buf);
    return 0;
}
