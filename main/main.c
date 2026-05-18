#include "project_inc.h"

static const char *TAG = "main";

#define MAIN_LVGL_STACK_SIZE       (5U * 1024U)
#define MAIN_ESPNOW_EVENT_STACK    (4096U)
#define MAIN_ESPNOW_SEND_STACK     (4U * 1024U)
#define MAIN_HEARTBEAT_STACK       (2U * 1024U)
#define MAIN_LED_STACK             (4U * 1024U)
#define MAIN_LVGL_PRIORITY         (3U)
#define MAIN_ESPNOW_EVENT_PRIORITY (4U)
#define MAIN_ESPNOW_SEND_PRIORITY  (3U)
#define MAIN_HEARTBEAT_PRIORITY    (3U)
#define MAIN_LED_PRIORITY          (3U)
#define MAIN_CORE_WIFI             (0)
#define MAIN_CORE_UI               (1)
#define MAIN_TASK_COUNT            (5U)

typedef struct {
    TaskFunction_t entry;
    const char *name;
    uint32_t stack_depth;
    UBaseType_t priority;
    BaseType_t core_id;
} app_task_config_t;

static TaskHandle_t s_app_task_handles[MAIN_TASK_COUNT];

/**
 * @brief 初始化 NVS 存储。
 *
 * @return 成功返回 ESP_OK，失败返回具体的 ESP-IDF 错误码。
 * @note Wi-Fi/ESP-NOW 依赖 NVS。若 NVS 分区满或版本不兼容，
 *       本函数会先擦除再重新初始化。
 */
static esp_err_t nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    }

    return ret;
}

/**
 * @brief 删除已经创建成功的应用任务。
 *
 * @param created_count 已创建任务数量。
 * @note 仅用于启动失败回滚路径，避免释放 ESP-NOW 资源后任务仍在运行。
 */
static void app_delete_created_tasks(uint32_t created_count)
{
    for (uint32_t i = 0U; i < created_count; i++) {
        if (s_app_task_handles[i] != NULL) {
            vTaskDelete(s_app_task_handles[i]);
            s_app_task_handles[i] = NULL;
        }
    }
}

/**
 * @brief 创建所有长期运行的应用任务。
 *
 * @return 全部任务创建成功返回 ESP_OK，否则返回 ESP_FAIL。
 * @note 任务固定到指定核心运行；若中途失败，会删除之前已创建的任务。
 */
static esp_err_t app_create_tasks(void)
{
    static const app_task_config_t task_table[MAIN_TASK_COUNT] = {
        {
            .entry = lvgl_task,
            .name = "lvgl_task",
            .stack_depth = MAIN_LVGL_STACK_SIZE,
            .priority = MAIN_LVGL_PRIORITY,
            .core_id = MAIN_CORE_UI,
        },
        {
            .entry = example_espnow_slave_event_task,
            .name = "espnow_slave_event",
            .stack_depth = MAIN_ESPNOW_EVENT_STACK,
            .priority = MAIN_ESPNOW_EVENT_PRIORITY,
            .core_id = MAIN_CORE_WIFI,
        },
        {
            .entry = example_espnow_slave_send_task,
            .name = "espnow_slave_send",
            .stack_depth = MAIN_ESPNOW_SEND_STACK,
            .priority = MAIN_ESPNOW_SEND_PRIORITY,
            .core_id = MAIN_CORE_WIFI,
        },
        {
            .entry = example_espnow_slave_heartbeat_check_task,
            .name = "espnow_slave_heartbeat",
            .stack_depth = MAIN_HEARTBEAT_STACK,
            .priority = MAIN_HEARTBEAT_PRIORITY,
            .core_id = MAIN_CORE_WIFI,
        },
        {
            .entry = led_task,
            .name = "led_task",
            .stack_depth = MAIN_LED_STACK,
            .priority = MAIN_LED_PRIORITY,
            .core_id = MAIN_CORE_UI,
        },
    };

    for (uint32_t i = 0U; i < MAIN_TASK_COUNT; i++) {
        BaseType_t ret = xTaskCreatePinnedToCore(task_table[i].entry,
                                                 task_table[i].name,
                                                 task_table[i].stack_depth,
                                                 NULL,
                                                 task_table[i].priority,
                                                 &s_app_task_handles[i],
                                                 task_table[i].core_id);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create task: %s", task_table[i].name);
            app_delete_created_tasks(i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/**
 * @brief ESP-IDF 应用入口函数。
 *
 * @note 启动顺序必须保持为 NVS、Wi-Fi、ESP-NOW、业务任务。
 *       任一步失败都会停止后续启动，并清理已初始化资源。
 */
void app_main(void)
{
    esp_err_t ret = nvs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    example_wifi_init();

    ret = example_espnow_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %s", esp_err_to_name(ret));
        example_espnow_deinit();
        return;
    }

    ret = app_create_tasks();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Task startup failed: %s", esp_err_to_name(ret));
        example_espnow_deinit();
        return;
    }

    ESP_LOGI(TAG,
             "Slave started, device id: %lu",
             (unsigned long)SLAVE_MAGIC_NUMBER);
}
