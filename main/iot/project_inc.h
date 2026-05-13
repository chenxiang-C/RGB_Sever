#ifndef PROJECT_INC_H
#define PROJECT_INC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lvgl.h"

#include "lcd.h"
#include "touch.h"

#include "my_lvgl.h"
#include "now.h"
#include "ws2812b.h"

#ifdef __cplusplus
}
#endif

#endif /* PROJECT_INC_H */
