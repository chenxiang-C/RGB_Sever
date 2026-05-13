#include "project_inc.h"

static const char *TAG = "ESPNOW_SLAVE";

#define ESPNOW_QUEUE_SIZE             (5U)
#define ESPNOW_QUEUE_SEND_TICKS       (0U)
#define ESPNOW_SEND_BUFFER_SIZE       (128U)
#define ESPNOW_PAYLOAD_STRING_SIZE    (32U)
#define ESPNOW_TX_STRING_SIZE         (64U)
#define ESPNOW_HEARTBEAT_INTERVAL_MS  (6000U)
#define ESPNOW_HOST_TIMEOUT_MS        (16000U)
#define ESPNOW_TASK_IDLE_DELAY_MS     (1000U)
#define ESPNOW_READY_STATE            (1U)

#define EXAMPLE_ESPNOW_DATA_BROADCAST (0U)
#define EXAMPLE_ESPNOW_DATA_UNICAST   (1U)

typedef struct {
    uint8_t type;
    uint8_t state;
    uint16_t seq_num;
    uint16_t crc;
    uint32_t magic;
    uint8_t payload[0];
} example_espnow_data_t;

typedef enum {
    EXAMPLE_ESPNOW_SEND_CB = 0,
    EXAMPLE_ESPNOW_RECV_CB
} example_espnow_event_id_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} example_espnow_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int32_t data_len;
} example_espnow_event_recv_cb_t;

typedef union {
    example_espnow_event_send_cb_t send_cb;
    example_espnow_event_recv_cb_t recv_cb;
} example_espnow_event_info_t;

typedef struct {
    example_espnow_event_id_t id;
    example_espnow_event_info_t info;
} example_espnow_event_t;

typedef struct {
    uint32_t magic;
    int32_t len;
    uint8_t *buffer;
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];
} example_espnow_send_param_t;

typedef struct {
    uint16_t active_part;
    bool data_send_mode;
    uint32_t last_host_msg_tick;
    uint8_t last_r;
    uint8_t last_g;
    uint8_t last_b;
    uint16_t last_part;
} espnow_slave_state_t;

static QueueHandle_t s_espnow_queue;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_send_mutex;
static uint16_t s_espnow_seq;
static example_espnow_send_param_t *s_send_param;
static espnow_slave_state_t s_slave_state = {
    .last_r = UINT8_MAX,
    .last_g = UINT8_MAX,
    .last_b = UINT8_MAX,
};

/**
 * @brief 加锁保护 ESP-NOW 从机运行状态。
 *
 * @param wait_ticks 等待互斥锁的最大 tick 数。
 * @return 获取锁成功返回 true，否则返回 false。
 * @note 该锁只保护 s_slave_state，不保护发送缓冲区。
 */
static bool espnow_state_lock(TickType_t wait_ticks)
{
    return (s_state_mutex != NULL) &&
           (xSemaphoreTake(s_state_mutex, wait_ticks) == pdTRUE);
}

/**
 * @brief 释放 ESP-NOW 从机运行状态锁。
 */
static void espnow_state_unlock(void)
{
    if (s_state_mutex != NULL) {
        xSemaphoreGive(s_state_mutex);
    }
}

/**
 * @brief 获取当前系统 tick 对应的毫秒时间。
 *
 * @return 当前 FreeRTOS tick 转换后的毫秒值。
 */
static uint32_t espnow_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief 查询当前是否处于颜色数据上报模式。
 *
 * @return true 表示上报颜色数据，false 表示仅发送待机心跳。
 */
static bool espnow_get_mode(void)
{
    bool enabled = false;

    if (espnow_state_lock(portMAX_DELAY)) {
        enabled = s_slave_state.data_send_mode;
        espnow_state_unlock();
    }

    return enabled;
}

/**
 * @brief 获取当前主机指定的 part 编号。
 *
 * @return 当前有效 part；0 表示未分配或待机。
 */
static uint16_t espnow_get_active_part(void)
{
    uint16_t active_part = 0U;

    if (espnow_state_lock(portMAX_DELAY)) {
        active_part = s_slave_state.active_part;
        espnow_state_unlock();
    }

    return active_part;
}

/**
 * @brief 刷新最近一次收到主机合法数据的时间戳。
 *
 * @note 仅在接收数据完成 CRC 和格式校验后调用。
 */
static void espnow_mark_host_alive(void)
{
    if (espnow_state_lock(portMAX_DELAY)) {
        s_slave_state.last_host_msg_tick = espnow_now_ms();
        espnow_state_unlock();
    }
}

/**
 * @brief 判断当前 UI 颜色或 part 是否相比上次发送发生变化。
 *
 * @param part 当前 part 编号。
 * @param color 当前 UI RGB 颜色指针，不能为 NULL。
 * @return 发生变化返回 true；未变化返回 false。
 * @note 检测到变化时会同步更新历史缓存，用于下一次去重判断。
 */
static bool espnow_color_changed(uint16_t part, const ui_rgb_color_t *color)
{
    bool changed = false;

    if (color == NULL) {
        return false;
    }

    if (espnow_state_lock(portMAX_DELAY)) {
        changed = (part != s_slave_state.last_part) ||
                  (color->r != s_slave_state.last_r) ||
                  (color->g != s_slave_state.last_g) ||
                  (color->b != s_slave_state.last_b);
        if (changed) {
            s_slave_state.last_part = part;
            s_slave_state.last_r = color->r;
            s_slave_state.last_g = color->g;
            s_slave_state.last_b = color->b;
        }
        espnow_state_unlock();
    }

    return changed;
}

/**
 * @brief 接收并应用主机下发的有效 part 编号。
 *
 * @param part 主机分配的 part 编号，必须大于 0。
 * @note 会切换到颜色上报模式，并强制清空颜色去重缓存，
 *       确保新 part 至少上报一次真实颜色。
 */
static void espnow_accept_part(uint16_t part)
{
    if (espnow_state_lock(portMAX_DELAY)) {
        s_slave_state.active_part = part;
        s_slave_state.data_send_mode = true;
        s_slave_state.last_r = UINT8_MAX;
        s_slave_state.last_g = UINT8_MAX;
        s_slave_state.last_b = UINT8_MAX;
        s_slave_state.last_part = 0U;
        espnow_state_unlock();
    }

    ui_update_part_id(part);
    ui_update_slave_status(true);
}

/**
 * @brief 将主机连接状态复位到待机模式。
 *
 * @param current_tick 当前毫秒时间戳，用作新的超时基准。
 * @note 超时检测任务调用本函数，同时更新 UI 为离线状态。
 */
static void espnow_reset_host_state(uint32_t current_tick)
{
    if (espnow_state_lock(portMAX_DELAY)) {
        s_slave_state.active_part = 0U;
        s_slave_state.data_send_mode = false;
        s_slave_state.last_host_msg_tick = current_tick;
        espnow_state_unlock();
    }

    ui_update_part_id(0U);
    ui_update_slave_status(false);
}

/**
 * @brief 校验并解析 ESP-NOW 协议帧头。
 *
 * @param data 接收缓冲区，函数会临时清零 crc 字段用于校验。
 * @param data_len 接收数据总长度。
 * @param state 输出协议 state 字段。
 * @param seq 输出协议序号。
 * @param magic 输出协议 magic 字段。
 * @return 成功返回协议 type，失败返回 -1。
 * @note 调用者必须传入可写缓冲区，因为 CRC 校验会临时修改 crc 字段后恢复。
 */
static int32_t example_espnow_data_parse(uint8_t *data,
                                         uint16_t data_len,
                                         uint8_t *state,
                                         uint16_t *seq,
                                         uint32_t *magic)
{
    if ((data == NULL) || (state == NULL) || (seq == NULL) ||
        (magic == NULL) || (data_len < sizeof(example_espnow_data_t))) {
        return -1;
    }

    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t crc_recv = buf->crc;

    buf->crc = 0U;
    uint16_t crc_cal = esp_crc16_le(UINT16_MAX,
                                    (const uint8_t *)buf,
                                    data_len);
    buf->crc = crc_recv;

    if (crc_cal != crc_recv) {
        return -1;
    }

    *state = buf->state;
    *seq = buf->seq_num;
    *magic = buf->magic;
    return (int32_t)buf->type;
}

/**
 * @brief 使用统一协议头封装 payload 并发送给主机。
 *
 * @param payload 以 '\0' 结尾的业务载荷。
 * @param payload_len 载荷字节数，必须包含结尾 '\0'。
 * @param region 业务 part 编号；0 表示心跳或错误类消息。
 * @return ESP_OK 表示发送请求已提交到底层 ESP-NOW。
 * @note s_send_param->buffer 是共享发送缓冲区，必须由 s_send_mutex 串行化。
 */
static esp_err_t espnow_send_payload(const char *payload,
                                     size_t payload_len,
                                     uint16_t region)
{
    if ((payload == NULL) ||
        (payload_len == 0U) ||
        (payload_len > ESPNOW_SEND_BUFFER_SIZE -
                       sizeof(example_espnow_data_t)) ||
        (s_send_param == NULL) ||
        (s_send_param->buffer == NULL) ||
        (s_send_mutex == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_send_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    example_espnow_data_t *esp_data =
        (example_espnow_data_t *)s_send_param->buffer;
    s_send_param->len = (int32_t)(sizeof(example_espnow_data_t) +
                                 payload_len);

    esp_data->type = EXAMPLE_ESPNOW_DATA_UNICAST;
    esp_data->state = ESPNOW_READY_STATE;
    esp_data->seq_num = s_espnow_seq++;
    esp_data->magic = s_send_param->magic;
    esp_data->crc = 0U;

    memcpy(esp_data->payload, payload, payload_len);
    esp_data->crc = esp_crc16_le(UINT16_MAX,
                                 (const uint8_t *)esp_data,
                                 s_send_param->len);

    esp_err_t ret = esp_now_send(s_send_param->dest_mac,
                                 s_send_param->buffer,
                                 s_send_param->len);
    if (ret == ESP_OK) {
        if (region == 0U) {
            ESP_LOGD(TAG, "Heartbeat packet sent");
        } else {
            ESP_LOGI(TAG,
                     "Report sent: %s | seq:%u",
                     payload,
                     esp_data->seq_num);
        }
    } else {
        ESP_LOGW(TAG, "ESP-NOW send failed: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_send_mutex);
    return ret;
}

/**
 * @brief 组装并发送颜色上报/心跳数据。
 *
 * @param region part 编号；0 表示心跳。
 * @param r 红色通道值。
 * @param g 绿色通道值。
 * @param b 蓝色通道值。
 * @return ESP_OK 表示发送请求提交成功。
 */
static esp_err_t slave_send_data(uint16_t region,
                                 uint8_t r,
                                 uint8_t g,
                                 uint8_t b)
{
    char send_str[ESPNOW_TX_STRING_SIZE] = {0};
    int32_t str_len = snprintf(send_str,
                               sizeof(send_str),
                               "part: %u,R:%u,G:%u,B:%u",
                               region,
                               r,
                               g,
                               b);

    if ((str_len <= 0) || ((size_t)str_len >= sizeof(send_str))) {
        return ESP_ERR_INVALID_SIZE;
    }

    return espnow_send_payload(send_str, (size_t)str_len + 1U, region);
}

/**
 * @brief 向主机上报接收数据异常。
 *
 * @return ESP_OK 表示错误消息发送请求提交成功。
 */
static esp_err_t slave_send_error_msg(void)
{
    char send_str[ESPNOW_TX_STRING_SIZE] = {0};
    int32_t str_len = snprintf(send_str,
                               sizeof(send_str),
                               "%lu:error",
                               (unsigned long)SLAVE_MAGIC_NUMBER);

    if ((str_len <= 0) || ((size_t)str_len >= sizeof(send_str))) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = espnow_send_payload(send_str,
                                        (size_t)str_len + 1U,
                                        0U);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Error report sent: %s", send_str);
    }

    return ret;
}

/**
 * @brief 检查 UI 颜色变化并决定发送颜色数据或心跳。
 *
 * @note 在颜色上报模式下周期调用。颜色未变化时发送 part:0 心跳，
 *       防止主机误判从机掉线，同时减少重复颜色上报。
 */
static void check_and_send_color_data(void)
{
    ui_rgb_color_t color = {0};
    uint16_t active_part = espnow_get_active_part();

    if (!ui_get_color(&color)) {
        ESP_LOGW(TAG, "Unable to read UI color, send heartbeat instead");
        (void)slave_send_data(0U, 0U, 0U, 0U);
        return;
    }

    if (!espnow_color_changed(active_part, &color)) {
        (void)slave_send_data(0U, 0U, 0U, 0U);
        return;
    }

    (void)slave_send_data(active_part, color.r, color.g, color.b);
}

/**
 * @brief ESP-NOW 发送完成回调。
 *
 * @param mac_addr 目标 MAC 地址。
 * @param status 发送结果状态。
 * @note 运行在 ESP-NOW/Wi-Fi 回调上下文，只投递事件，不做耗时处理。
 */
static void example_espnow_send_cb(const uint8_t *mac_addr,
                                   esp_now_send_status_t status)
{
    if ((mac_addr == NULL) || (s_espnow_queue == NULL)) {
        return;
    }

    example_espnow_event_t evt = {
        .id = EXAMPLE_ESPNOW_SEND_CB,
    };

    memcpy(evt.info.send_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    evt.info.send_cb.status = status;
    (void)xQueueSend(s_espnow_queue, &evt, ESPNOW_QUEUE_SEND_TICKS);
}

/**
 * @brief ESP-NOW 接收回调。
 *
 * @param recv_ctx 接收上下文，包含源 MAC。
 * @param data 底层接收缓冲区。
 * @param len 数据长度。
 * @note 仅接收 HOST_MAC_ADDR 的数据；会复制 payload 到堆内存，
 *       所有权转交给 example_espnow_slave_event_task() 释放。
 */
static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_ctx,
                                   const uint8_t *data,
                                   int len)
{
    const uint8_t host_mac[ESP_NOW_ETH_ALEN] = HOST_MAC_ADDR;

    if ((recv_ctx == NULL) || (data == NULL) || (len <= 0) ||
        (s_espnow_queue == NULL)) {
        return;
    }

    if (memcmp(recv_ctx->src_addr, host_mac, ESP_NOW_ETH_ALEN) != 0) {
        return;
    }

    example_espnow_event_t evt = {
        .id = EXAMPLE_ESPNOW_RECV_CB,
    };
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

    memcpy(recv_cb->mac_addr, recv_ctx->src_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = (uint8_t *)malloc((size_t)len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "RX buffer allocation failed");
        return;
    }

    memcpy(recv_cb->data, data, (size_t)len);
    recv_cb->data_len = (int32_t)len;

    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_QUEUE_SEND_TICKS) != pdTRUE) {
        free(recv_cb->data);
        recv_cb->data = NULL;
        ESP_LOGW(TAG, "ESP-NOW event queue is full");
    }
}

/**
 * @brief 释放 ESP-NOW 模块已创建的资源。
 *
 * @note 调用前应确保 ESP-NOW 相关任务不会再访问这些资源。
 */
void example_espnow_deinit(void)
{
    if (s_send_param != NULL) {
        free(s_send_param->buffer);
        s_send_param->buffer = NULL;
        free(s_send_param);
        s_send_param = NULL;
    }

    if (s_espnow_queue != NULL) {
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
    }

    if (s_send_mutex != NULL) {
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
    }

    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }

    (void)esp_now_deinit();
}

/**
 * @brief ESP-NOW 周期发送任务。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用。
 * @note data_send_mode 为 false 时发送慢心跳；为 true 时读取 UI 颜色并上报。
 */
void example_espnow_slave_send_task(void *pvParameter)
{
    (void)pvParameter;

    while (true) {
        if ((s_send_param == NULL) || (s_send_param->buffer == NULL)) {
            vTaskDelay(pdMS_TO_TICKS(ESPNOW_TASK_IDLE_DELAY_MS));
            continue;
        }

        if (espnow_get_mode()) {
            check_and_send_color_data();
            vTaskDelay(pdMS_TO_TICKS(SLAVE_SEND_INTERVAL_MS));
        } else {
            (void)slave_send_data(0U, 0U, 0U, 0U);
            vTaskDelay(pdMS_TO_TICKS(ESPNOW_HEARTBEAT_INTERVAL_MS));
        }
    }
}

/**
 * @brief 解析业务 payload 中的 part 指令。
 *
 * @param payload 已补 '\0' 的字符串，格式应为 "part:<number>"。
 * @note part 为 0 时仅刷新在线状态；part 大于 0 时进入颜色上报模式。
 */
static void espnow_handle_part_payload(const char *payload)
{
    int32_t temp_part = 0;

    if (sscanf(payload, "part:%" SCNd32, &temp_part) != 1) {
        ESP_LOGW(TAG, "Invalid payload format: '%s'", payload);
        (void)slave_send_error_msg();
        return;
    }

    if (temp_part < 0) {
        ESP_LOGW(TAG, "Invalid negative part id: %" PRId32, temp_part);
        (void)slave_send_error_msg();
        return;
    }

    if (temp_part > UINT16_MAX) {
        ESP_LOGW(TAG, "Part id is too large: %" PRId32, temp_part);
        (void)slave_send_error_msg();
        return;
    }

    if (temp_part == 0) {
        ui_update_slave_status(true);
        ESP_LOGD(TAG, "Host heartbeat received");
        return;
    }

    espnow_accept_part((uint16_t)temp_part);
    ESP_LOGI(TAG, "Part parsed: %" PRId32 ", report mode enabled", temp_part);
}

/**
 * @brief 处理一个 ESP-NOW 接收事件。
 *
 * @param recv_cb 接收事件数据，data 字段必须有效。
 * @note 本函数只解析和处理数据，不释放 recv_cb->data；
 *       释放动作由事件任务统一完成。
 */
static void espnow_handle_recv_event(example_espnow_event_recv_cb_t *recv_cb)
{
    uint8_t recv_state = 0U;
    uint16_t recv_seq = 0U;
    uint32_t recv_magic = 0U;

    if ((recv_cb == NULL) || (recv_cb->data == NULL)) {
        return;
    }

    int32_t ret = example_espnow_data_parse(recv_cb->data,
                                            (uint16_t)recv_cb->data_len,
                                            &recv_state,
                                            &recv_seq,
                                            &recv_magic);
    if ((ret != EXAMPLE_ESPNOW_DATA_UNICAST) ||
        (recv_cb->data_len < (int32_t)sizeof(example_espnow_data_t))) {
        (void)slave_send_error_msg();
        return;
    }

    int32_t payload_len =
        recv_cb->data_len - (int32_t)sizeof(example_espnow_data_t);
    if ((payload_len <= 0) ||
        (payload_len >= (int32_t)ESPNOW_PAYLOAD_STRING_SIZE)) {
        ESP_LOGW(TAG, "Invalid payload length: %" PRId32, payload_len);
        (void)slave_send_error_msg();
        return;
    }

    const example_espnow_data_t *esp_data =
        (const example_espnow_data_t *)recv_cb->data;
    char payload_str[ESPNOW_PAYLOAD_STRING_SIZE] = {0};

    memcpy(payload_str, esp_data->payload, (size_t)payload_len);
    payload_str[payload_len] = '\0';

    (void)recv_state;
    (void)recv_seq;
    (void)recv_magic;
    espnow_mark_host_alive();
    espnow_handle_part_payload(payload_str);
}

/**
 * @brief ESP-NOW 回调事件消费任务。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用。
 * @note 所有接收缓冲区在本任务内释放，避免回调上下文做复杂处理。
 */
void example_espnow_slave_event_task(void *pvParameter)
{
    (void)pvParameter;
    example_espnow_event_t evt = {0};

    while (true) {
        if (s_espnow_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(ESPNOW_TASK_IDLE_DELAY_MS));
            continue;
        }

        if (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (evt.id) {
        case EXAMPLE_ESPNOW_SEND_CB:
            break;

        case EXAMPLE_ESPNOW_RECV_CB:
            espnow_handle_recv_event(&evt.info.recv_cb);
            free(evt.info.recv_cb.data);
            evt.info.recv_cb.data = NULL;
            break;

        default:
            ESP_LOGW(TAG, "Unknown ESP-NOW event: %d", (int)evt.id);
            break;
        }
    }
}

/**
 * @brief 主机心跳超时检测任务。
 *
 * @param pvParameter FreeRTOS 任务参数，当前未使用。
 * @note 周期检查最近主机消息时间；超时后复位业务状态和 UI 状态。
 */
void example_espnow_slave_heartbeat_check_task(void *pvParameter)
{
    (void)pvParameter;

    if (espnow_state_lock(portMAX_DELAY)) {
        s_slave_state.last_host_msg_tick = espnow_now_ms();
        espnow_state_unlock();
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ESPNOW_TASK_IDLE_DELAY_MS));

        uint32_t current_tick = espnow_now_ms();
        uint32_t last_tick = 0U;

        if (espnow_state_lock(portMAX_DELAY)) {
            last_tick = s_slave_state.last_host_msg_tick;
            espnow_state_unlock();
        }

        if ((current_tick - last_tick) > ESPNOW_HOST_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Host timeout, reset to idle mode");
            espnow_reset_host_state(current_tick);
        }
    }
}

/**
 * @brief 初始化 Wi-Fi STA 模式并固定 ESP-NOW 信道。
 *
 * @note 必须在 example_espnow_init() 前调用。
 */
void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL,
                                         WIFI_SECOND_CHAN_NONE));

    uint8_t slave_mac[ESP_NOW_ETH_ALEN] = {0};
    esp_read_mac(slave_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "WiFi ready | STA MAC: " MACSTR, MAC2STR(slave_mac));
}

/**
 * @brief 创建 ESP-NOW 模块运行所需的队列、锁和发送缓冲区。
 *
 * @return 成功返回 ESP_OK，内存不足返回 ESP_ERR_NO_MEM。
 * @note 失败时由上层 example_espnow_init() 统一调用 deinit 清理。
 */
static esp_err_t espnow_create_resources(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (s_send_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE,
                                  sizeof(example_espnow_event_t));
    if (s_espnow_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_send_param = (example_espnow_send_param_t *)calloc(
        1U,
        sizeof(example_espnow_send_param_t));
    if (s_send_param == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_send_param->buffer = (uint8_t *)malloc(ESPNOW_SEND_BUFFER_SIZE);
    if (s_send_param->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief 初始化 ESP-NOW 并注册发送/接收回调。
 *
 * @return 成功返回 ESP_OK，否则返回底层 ESP-NOW 错误码。
 */
static esp_err_t espnow_register_callbacks(void)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_now_register_send_cb(example_espnow_send_cb);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_now_register_recv_cb(example_espnow_recv_cb);
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_now_set_pmk((const uint8_t *)ESPNOW_PMK);
}

/**
 * @brief 将固定主机 MAC 加入 ESP-NOW peer 列表。
 *
 * @return 成功返回 ESP_OK，否则返回 esp_now_add_peer() 的错误码。
 */
static esp_err_t espnow_add_host_peer(void)
{
    const uint8_t host_mac[ESP_NOW_ETH_ALEN] = HOST_MAC_ADDR;
    esp_now_peer_info_t peer = {0};

    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, host_mac, ESP_NOW_ETH_ALEN);

    return esp_now_add_peer(&peer);
}

/**
 * @brief 初始化 ESP-NOW 从机模块。
 *
 * @return 成功返回 ESP_OK，失败返回具体错误码。
 * @note 初始化成功后才允许创建 ESP-NOW 发送、接收和超时检测任务。
 */
esp_err_t example_espnow_init(void)
{
    const uint8_t host_mac[ESP_NOW_ETH_ALEN] = HOST_MAC_ADDR;
    esp_err_t ret = espnow_create_resources();

    if (ret != ESP_OK) {
        example_espnow_deinit();
        return ret;
    }

    ret = espnow_register_callbacks();
    if (ret != ESP_OK) {
        example_espnow_deinit();
        return ret;
    }

    ret = espnow_add_host_peer();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add host peer: " MACSTR,
                 MAC2STR(host_mac));
        example_espnow_deinit();
        return ret;
    }

    memcpy(s_send_param->dest_mac, host_mac, ESP_NOW_ETH_ALEN);
    s_send_param->magic = SLAVE_MAGIC_NUMBER;

    ESP_LOGI(TAG,
             "ESP-NOW slave ready | id:%lu host:" MACSTR,
             (unsigned long)SLAVE_MAGIC_NUMBER,
             MAC2STR(host_mac));
    return ESP_OK;
}
