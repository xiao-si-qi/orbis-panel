/**
 * @file voice_audio.h
 * @brief 语音硬件通路：I2S 麦克风录音 + I2S DAC 播放（阶段④的音频底层）。
 *
 * 板子（GUITION JC3636W518 / 太极派 S3）音频引脚：
 *   麦克风（I2S 输入）：WS=45, SCK=42, SD=46
 *   喇叭（PCM5100A，I2S 输出）：BCLK=18, WS=16, DOUT=17，静音脚 GPIO48（低有效）
 */
#ifndef VOICE_AUDIO_H
#define VOICE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/** 初始化麦克风 + 喇叭（重复调用安全）。 */
int voice_audio_init(void);

/** 是否已初始化。 */
int voice_audio_ready(void);

/** 麦克风录音到 PSRAM 缓冲，返回实际采样点数（16bit mono）。 */
int voice_audio_record(int16_t *buf, int max_samples, int sample_rate, int ms);

/** 播放一段 16bit mono PCM。 */
int voice_audio_play(const int16_t *buf, int samples, int sample_rate);

/** 静音/解除静音（PCM5100A 的 mute 脚）。 */
void voice_audio_mute(int on);

/**
 * @brief 自检：录 1.5 秒 → 统计峰值/RMS → 回放。
 *
 * 用于验证麦克风和喇叭接线是否正常（日志里看 peak/rms）。
 * @param peak 输出峰值（0-32767）
 * @param rms  输出均方根
 */
int voice_audio_selftest(int *peak, int *rms);

#endif /* VOICE_AUDIO_H */
