#ifndef NOW_H
#define NOW_H

#include <stdint.h>

#include "esp_err.h"

#define SLAVE_MAGIC_NUMBER      (0x4U)
#define ESPNOW_PMK              "ESP32_HOST_SLAVE_PMK"
#define ESPNOW_CHANNEL          (1U)
#define SLAVE_SEND_INTERVAL_MS  (3000U)

/**
 * @brief 初始化 Wi-Fi STA 模式并设置 ESP-NOW 通信信道。
 *
 * @note 必须在 example_espnow_init() 之前调用。
 *       当前函数内部使用 ESP_ERROR_CHECK，底层初始化失败会触发异常退出。
 */
void example_wifi_init(void);

/**
 * @brief 初始化 ESP-NOW 从机通信模块。
 *
 * @return 成功返回 ESP_OK，失败返回具体错误码。
 * @note 本函数会创建事件队列、互斥锁、发送缓冲区，注册 ESP-NOW 回调，
 *       并把主机 MAC 加入 peer 列表。失败时会自动释放已申请资源。
 */
esp_err_t example_espnow_init(void);

/**
 * @brief 释放 ESP-NOW 从机模块资源。
 *
 * @note 会释放发送缓冲区、事件队列、互斥锁，并调用 esp_now_deinit()。
 *       应在相关任务停止后调用，避免任务继续访问已释放资源。
 */
void example_espnow_deinit(void);

/**
 * @brief ESP-NOW 事件处理任务入口。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用，传 NULL 即可。
 * @note 负责消费 ESP-NOW 回调投递的发送/接收事件。
 *       接收数据缓冲区的释放也在此任务中完成。
 */
void example_espnow_slave_event_task(void *pvParameter);

/**
 * @brief ESP-NOW 周期发送任务入口。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用，传 NULL 即可。
 * @note 未连接主机时发送慢心跳；收到有效 part 后进入颜色数据上报模式。
 */
void example_espnow_slave_send_task(void *pvParameter);

/**
 * @brief 主机在线状态检测任务入口。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用，传 NULL 即可。
 * @note 若超过 ESPNOW_HOST_TIMEOUT_MS 未收到主机数据，会清空 part 并退回待机。
 */
void example_espnow_slave_heartbeat_check_task(void *pvParameter);

#endif /* NOW_H */
