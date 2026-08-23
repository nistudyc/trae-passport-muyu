#include "woodfish_store.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORE_MAGIC    0x4D555955U   // "MUYU"
#define STORE_VERSION  1
#define STORE_NAMESPACE "muyu"
#define STORE_KEY      "state"
#define STORE_TONE_KEY "tone"
#define STORE_BL_KEY   "bl"
#define SAVE_DEBOUNCE_MS 3000

static const char *TAG = "wf_store";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t muted;
    uint8_t auto_mode;
    uint16_t reserved;
    uint32_t merit;
    uint32_t crc32;
} store_blob_t;

static nvs_handle_t s_nvs;
static bool s_ready;
static volatile bool s_error;

static SemaphoreHandle_t s_mtx;    // 保护 s_pending
static SemaphoreHandle_t s_wake;   // 唤醒保存任务
static woodfish_model_t s_pending;

static uint32_t crc32_bytes(const void *data, size_t len) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

static store_blob_t blob_from_model(const woodfish_model_t *model) {
    store_blob_t blob = {
        .magic = STORE_MAGIC,
        .version = STORE_VERSION,
        .size = sizeof(store_blob_t),
        .muted = model->muted ? 1 : 0,
        .auto_mode = model->auto_mode ? 1 : 0,
        .merit = model->merit,
    };
    blob.crc32 = crc32_bytes(&blob, offsetof(store_blob_t, crc32));
    return blob;
}

static bool model_from_blob(const store_blob_t *blob, woodfish_model_t *model) {
    if (blob->magic != STORE_MAGIC || blob->version != STORE_VERSION ||
        blob->size != sizeof(*blob) ||
        blob->crc32 != crc32_bytes(blob, offsetof(store_blob_t, crc32))) {
        return false;
    }
    woodfish_model_init(model);
    model->merit = blob->merit;
    model->muted = blob->muted != 0;
    model->auto_mode = blob->auto_mode != 0;
    return true;
}

static void save_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        // 防抖:静默期内的重复请求合并为最后一次写入。
        vTaskDelay(pdMS_TO_TICKS(SAVE_DEBOUNCE_MS));
        while (xSemaphoreTake(s_wake, 0) == pdTRUE) {}
        woodfish_model_t snap;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        snap = s_pending;
        xSemaphoreGive(s_mtx);

        store_blob_t blob = blob_from_model(&snap);
        if (nvs_set_blob(s_nvs, STORE_KEY, &blob, sizeof(blob)) == ESP_OK &&
            nvs_commit(s_nvs) == ESP_OK) {
            ESP_LOGI(TAG, "功德已保存: merit=%lu muted=%d auto=%d",
                     (unsigned long)snap.merit, snap.muted, snap.auto_mode);
        } else {
            s_error = true;
            ESP_LOGE(TAG, "NVS 写入失败,本次功德改动可能未持久化");
        }
    }
}

bool woodfish_store_init(woodfish_model_t *model) {
    woodfish_model_init(model);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = 已被别人初始化
        s_error = true;
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return false;
    }

    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "nvs_open 失败,功德将不持久化");
        return false;
    }

    store_blob_t blob = {0};
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_nvs, STORE_KEY, &blob, &len) == ESP_OK) {
        if (model_from_blob(&blob, model)) {
            ESP_LOGI(TAG, "载入功德: merit=%lu muted=%d auto=%d",
                     (unsigned long)model->merit, model->muted, model->auto_mode);
        } else {
            ESP_LOGW(TAG, "存储数据校验失败,从零开始积功德");
        }
    }

    s_mtx = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_mtx || !s_wake) {
        s_error = true;
        ESP_LOGE(TAG, "信号量创建失败");
        return false;
    }

    if (xTaskCreate(save_task, "wf_store", 3072, NULL, 3, NULL) != pdPASS) {
        s_error = true;
        ESP_LOGE(TAG, "保存任务创建失败");
        return false;
    }

    s_ready = true;
    return true;
}

void woodfish_store_request_save(const woodfish_model_t *model) {
    if (!s_ready) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pending = *model;
    xSemaphoreGive(s_mtx);
    xSemaphoreGive(s_wake);
}

bool woodfish_store_save_now(const woodfish_model_t *model) {
    if (!s_ready) return false;
    // 与防抖任务可能并发写同一键,NVS 内部有锁,后写者胜,均为完整 blob,安全。
    store_blob_t blob = blob_from_model(model);
    if (nvs_set_blob(s_nvs, STORE_KEY, &blob, sizeof(blob)) != ESP_OK ||
        nvs_commit(s_nvs) != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "NVS 同步写入失败");
        return false;
    }
    ESP_LOGI(TAG, "功德已同步落盘: merit=%lu", (unsigned long)model->merit);
    return true;
}

uint8_t woodfish_store_tone(void) {
    if (!s_ready) return 0;
    uint8_t val = 0;
    if (nvs_get_u8(s_nvs, STORE_TONE_KEY, &val) != ESP_OK) return 0;
    return val;
}

void woodfish_store_set_tone(uint8_t tone) {
    if (!s_ready) return;
    if (nvs_set_u8(s_nvs, STORE_TONE_KEY, tone) != ESP_OK ||
        nvs_commit(s_nvs) != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "音色偏好写入失败");
    }
}

uint8_t woodfish_store_brightness(void) {
    if (!s_ready) return 100;
    uint8_t val = 100;
    if (nvs_get_u8(s_nvs, STORE_BL_KEY, &val) != ESP_OK) return 100;
    if (val > 100) return 100;
    return val;
}

void woodfish_store_set_brightness(uint8_t percent) {
    if (!s_ready || percent > 100) return;
    if (nvs_set_u8(s_nvs, STORE_BL_KEY, percent) != ESP_OK ||
        nvs_commit(s_nvs) != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "亮度偏好写入失败");
    }
}

bool woodfish_store_has_error(void) {
    return s_error;
}
