#ifndef WS2812B_H
#define WS2812B_H

#include <stdint.h>

#include "driver/gpio.h"

#define WS2812B_GPIO_PIN  GPIO_NUM_25
#define WS2812B_LED_NUM   (6U)

/**
 * @brief 初始化 WS2812B 灯带 RMT 驱动。
 *
 * @note 需要在调用 ws2812b_set_color() 前执行一次。
 */
void ws2812b_init(void);

/**
 * @brief 设置整条 WS2812B 灯带颜色。
 *
 * @param r 红色通道值。
 * @param g 绿色通道值。
 * @param b 蓝色通道值。
 * @note 若灯带尚未初始化，本函数会直接返回。
 */
void ws2812b_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 灯带同步任务入口。
 *
 * @param pvParameters FreeRTOS 任务参数，当前未使用，传 NULL 即可。
 * @note 周期读取 UI 颜色，检测到变化后刷新灯带。
 */
void led_task(void *pvParameters);

#endif /* WS2812B_H */
