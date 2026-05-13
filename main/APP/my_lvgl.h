#ifndef MY_LVGL_H
#define MY_LVGL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ui_rgb_color_t;

/**
 * @brief LVGL UI 任务入口。
 *
 * @param pvParameters FreeRTOS 任务参数，当前未使用，传 NULL 即可。
 * @note 该任务内部初始化 LCD、触摸、LVGL tick 定时器并持续调用
 *       lv_timer_handler()。应用层只需要创建任务，不要直接调用。
 */
void lvgl_task(void *pvParameters);

/**
 * @brief 读取 UI 当前选择的 RGB 颜色。
 *
 * @param color 输出颜色结构体指针，不能为 NULL。
 * @return 读取成功返回 true；参数非法返回 false。
 * @note 本接口内部使用临界区保护，可被 ESP-NOW 任务或灯带任务调用。
 */
bool ui_get_color(ui_rgb_color_t *color);

/**
 * @brief 更新 UI 中显示的当前 part 编号。
 *
 * @param part_id 主机分配的 part 编号；0 表示空闲/未连接。
 * @note 可从非 LVGL 任务调用，内部会尝试获取 LVGL 互斥锁。
 */
void ui_update_part_id(uint16_t part_id);

/**
 * @brief 更新 UI 中显示的主机连接状态。
 *
 * @param is_connected true 表示在线，false 表示离线。
 * @note 可从 ESP-NOW 事件任务或心跳检测任务调用。
 */
void ui_update_slave_status(bool is_connected);

#ifdef __cplusplus
}
#endif

#endif /* MY_LVGL_H */
