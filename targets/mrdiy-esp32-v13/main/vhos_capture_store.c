#include "vhos_capture_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vhos_can.h"

#define VHOS_CAPTURE_BASE_PATH "/vhos"
#define VHOS_CAPTURE_CURRENT_PATH "/vhos/current.vhcan"
#define VHOS_CAPTURE_PREVIOUS_PATH "/vhos/previous.vhcan"
#define VHOS_CAPTURE_HEADER_BYTES 32U
#define VHOS_CAPTURE_MAX_FILE_BYTES (430U * 1024U)
#define VHOS_CAPTURE_QUEUE_DEPTH 192U
#define VHOS_CAPTURE_FLUSH_RECORDS 32U
#define VHOS_CAPTURE_CHANGED_INTERVAL_US 200000ULL
#define VHOS_CAPTURE_UNCHANGED_INTERVAL_US 1000000ULL
#define VHOS_CAPTURE_SAMPLE_BUCKETS 192U

typedef struct {
    bool used;
    bool extended;
    uint32_t identifier;
    uint32_t bitrate_bps;
    uint64_t last_retained_us;
    uint8_t last_length;
    uint8_t last_data[8];
} sample_bucket_t;

static const char *TAG = "vhos_capture";
static QueueHandle_t record_queue;
static SemaphoreHandle_t file_lock;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE sample_lock = portMUX_INITIALIZER_UNLOCKED;
static FILE *current_file;
static bool mounted;
static bool logging_enabled;
static uint32_t current_session_id;
static uint64_t observed_frames;
static uint64_t sampled_frames;
static uint64_t retained_records;
static uint64_t sample_suppressed_frames;
static uint64_t queue_dropped_records;
static uint64_t storage_write_failures;
static sample_bucket_t sample_buckets[VHOS_CAPTURE_SAMPLE_BUCKETS];

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void write_u64_le(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0; index < 8; index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint32_t crc32c(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82F63B78U : 0U);
        }
    }
    return ~crc;
}

static uint32_t next_session_id(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    uint32_t previous;
    portENTER_CRITICAL(&status_lock);
    previous = current_session_id;
    portEXIT_CRITICAL(&status_lock);
    uint32_t mixed = esp_random() ^ (uint32_t)now ^ (uint32_t)(now >> 32) ^
                     previous;
    return mixed == 0 ? 1 : mixed;
}

static uint32_t file_size(const char *path)
{
    struct stat information;
    if (stat(path, &information) != 0 || information.st_size < 0) {
        return 0;
    }
    return (uint32_t)information.st_size;
}

static uint32_t session_from_file(const char *path)
{
    uint8_t header[VHOS_CAPTURE_HEADER_BYTES];
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    size_t read = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (read != sizeof(header) || memcmp(header, "VHCL", 4) != 0 ||
        header[4] != 1 || header[5] != VHOS_CAPTURE_RECORD_BYTES ||
        read_u32_le(&header[28]) != crc32c(header, 28)) {
        return 0;
    }
    return read_u32_le(&header[8]);
}

static esp_err_t open_new_current_locked(void)
{
    uint32_t session_id = next_session_id();
    portENTER_CRITICAL(&status_lock);
    current_session_id = session_id;
    portEXIT_CRITICAL(&status_lock);
    current_file = fopen(VHOS_CAPTURE_CURRENT_PATH, "wb");
    if (current_file == NULL) {
        return ESP_FAIL;
    }
    setvbuf(current_file, NULL, _IOFBF, 4096);
    uint8_t header[VHOS_CAPTURE_HEADER_BYTES] = {0};
    memcpy(header, "VHCL", 4);
    header[4] = 1;
    header[5] = VHOS_CAPTURE_RECORD_BYTES;
    write_u32_le(&header[8], session_id);
    write_u64_le(&header[12], (uint64_t)esp_timer_get_time());
    write_u32_le(&header[20], VHOS_CAPTURE_CHANGED_INTERVAL_US);
    write_u32_le(&header[24], VHOS_CAPTURE_UNCHANGED_INTERVAL_US);
    write_u32_le(&header[28], crc32c(header, 28));
    if (fwrite(header, 1, sizeof(header), current_file) != sizeof(header) ||
        fflush(current_file) != 0) {
        fclose(current_file);
        current_file = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CAPTURE_SESSION_OPEN session=%lu", (unsigned long)session_id);
    return ESP_OK;
}

static esp_err_t rotate_locked(void)
{
    if (current_file != NULL) {
        fflush(current_file);
        fclose(current_file);
        current_file = NULL;
    }
    unlink(VHOS_CAPTURE_PREVIOUS_PATH);
    if (file_size(VHOS_CAPTURE_CURRENT_PATH) >= VHOS_CAPTURE_HEADER_BYTES &&
        rename(VHOS_CAPTURE_CURRENT_PATH, VHOS_CAPTURE_PREVIOUS_PATH) != 0) {
        ESP_LOGW(TAG, "Unable to retain previous capture segment");
    }
    portENTER_CRITICAL(&sample_lock);
    memset(sample_buckets, 0, sizeof(sample_buckets));
    portEXIT_CRITICAL(&sample_lock);
    return open_new_current_locked();
}

static void encode_record(const vhos_can_observation_t *observation, uint8_t *record)
{
    memset(record, 0, VHOS_CAPTURE_RECORD_BYTES);
    record[0] = 1;
    record[1] = (observation->extended ? 0x01 : 0) |
                (observation->remote_request ? 0x02 : 0) |
                (observation->listen_only ? 0x04 : 0);
    record[2] = observation->data_length;
    record[3] = observation->bitrate_bps == 250000U ? 2 : 1;
    write_u32_le(&record[4], observation->identifier);
    write_u64_le(&record[8], observation->source_sequence);
    write_u64_le(&record[16], observation->monotonic_us);
    memcpy(&record[24], observation->data, 8);
    write_u32_le(&record[32], crc32c(record, 32));
}

static sample_bucket_t *bucket_for(const vhos_can_observation_t *observation)
{
    uint32_t hash = observation->identifier ^ (observation->bitrate_bps >> 8) ^
                    (observation->extended ? 0x9E3779B9U : 0U);
    for (size_t probe = 0; probe < VHOS_CAPTURE_SAMPLE_BUCKETS; probe++) {
        sample_bucket_t *bucket = &sample_buckets[(hash + probe) % VHOS_CAPTURE_SAMPLE_BUCKETS];
        if (!bucket->used || (bucket->identifier == observation->identifier &&
            bucket->extended == observation->extended &&
            bucket->bitrate_bps == observation->bitrate_bps)) {
            return bucket;
        }
    }
    return &sample_buckets[hash % VHOS_CAPTURE_SAMPLE_BUCKETS];
}

static void observe_can(const vhos_can_observation_t *observation, void *context)
{
    (void)context;
    bool enabled;
    portENTER_CRITICAL(&status_lock);
    observed_frames++;
    enabled = mounted && logging_enabled;
    portEXIT_CRITICAL(&status_lock);
    if (!enabled || observation->remote_request) {
        return;
    }

    portENTER_CRITICAL(&sample_lock);
    sample_bucket_t *bucket = bucket_for(observation);
    bool same_key = bucket->used && bucket->identifier == observation->identifier &&
                    bucket->extended == observation->extended &&
                    bucket->bitrate_bps == observation->bitrate_bps;
    bool changed = !same_key || bucket->last_length != observation->data_length ||
                   memcmp(bucket->last_data, observation->data, 8) != 0;
    uint64_t elapsed = same_key
        ? observation->monotonic_us - bucket->last_retained_us
        : UINT64_MAX;
    uint64_t minimum = changed
        ? VHOS_CAPTURE_CHANGED_INTERVAL_US
        : VHOS_CAPTURE_UNCHANGED_INTERVAL_US;
    if (elapsed < minimum) {
        portEXIT_CRITICAL(&sample_lock);
        portENTER_CRITICAL(&status_lock);
        sample_suppressed_frames++;
        portEXIT_CRITICAL(&status_lock);
        return;
    }

    bucket->used = true;
    bucket->identifier = observation->identifier;
    bucket->extended = observation->extended;
    bucket->bitrate_bps = observation->bitrate_bps;
    bucket->last_retained_us = observation->monotonic_us;
    bucket->last_length = observation->data_length;
    memcpy(bucket->last_data, observation->data, 8);
    portEXIT_CRITICAL(&sample_lock);

    portENTER_CRITICAL(&status_lock);
    sampled_frames++;
    portEXIT_CRITICAL(&status_lock);
    if (xQueueSend(record_queue, observation, 0) != pdTRUE) {
        portENTER_CRITICAL(&status_lock);
        queue_dropped_records++;
        portEXIT_CRITICAL(&status_lock);
    }
}

static void writer_task(void *context)
{
    (void)context;
    vhos_can_observation_t observation;
    uint32_t records_since_flush = 0;
    while (true) {
        if (xQueueReceive(record_queue, &observation, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t record[VHOS_CAPTURE_RECORD_BYTES];
        encode_record(&observation, record);
        if (xSemaphoreTake(file_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
            portENTER_CRITICAL(&status_lock);
            storage_write_failures++;
            portEXIT_CRITICAL(&status_lock);
            continue;
        }
        if (current_file != NULL &&
            file_size(VHOS_CAPTURE_CURRENT_PATH) + VHOS_CAPTURE_RECORD_BYTES >
                VHOS_CAPTURE_MAX_FILE_BYTES) {
            if (rotate_locked() != ESP_OK) {
                portENTER_CRITICAL(&status_lock);
                storage_write_failures++;
                logging_enabled = false;
                portEXIT_CRITICAL(&status_lock);
            }
        }
        bool wrote = current_file != NULL &&
                     fwrite(record, 1, sizeof(record), current_file) == sizeof(record);
        if (wrote) {
            records_since_flush++;
            if (records_since_flush >= VHOS_CAPTURE_FLUSH_RECORDS) {
                wrote = fflush(current_file) == 0;
                records_since_flush = 0;
            }
        }
        xSemaphoreGive(file_lock);
        portENTER_CRITICAL(&status_lock);
        if (wrote) {
            retained_records++;
        } else {
            storage_write_failures++;
        }
        portEXIT_CRITICAL(&status_lock);
    }
}

esp_err_t vhos_capture_store_start(void)
{
    file_lock = xSemaphoreCreateMutex();
    record_queue = xQueueCreate(VHOS_CAPTURE_QUEUE_DEPTH, sizeof(vhos_can_observation_t));
    if (file_lock == NULL || record_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_vfs_spiffs_conf_t configuration = {
        .base_path = VHOS_CAPTURE_BASE_PATH,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t result = esp_vfs_spiffs_register(&configuration);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE_STORE_UNAVAILABLE reason=%s", esp_err_to_name(result));
        return result;
    }
    portENTER_CRITICAL(&status_lock);
    mounted = true;
    logging_enabled = true;
    portEXIT_CRITICAL(&status_lock);

    if (xSemaphoreTake(file_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    result = rotate_locked();
    xSemaphoreGive(file_lock);
    if (result != ESP_OK) {
        return result;
    }
    result = vhos_can_register_observer(observe_can, NULL);
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(writer_task, "vhos_capture", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        TAG,
        "CAPTURE_STORE_READY record_bytes=%u segment_limit=%u changed_interval_ms=%u unchanged_interval_ms=%u",
        VHOS_CAPTURE_RECORD_BYTES,
        VHOS_CAPTURE_MAX_FILE_BYTES,
        (unsigned)(VHOS_CAPTURE_CHANGED_INTERVAL_US / 1000),
        (unsigned)(VHOS_CAPTURE_UNCHANGED_INTERVAL_US / 1000)
    );
    return ESP_OK;
}

esp_err_t vhos_capture_store_get_status(vhos_capture_store_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    portENTER_CRITICAL(&status_lock);
    status->mounted = mounted;
    status->logging = logging_enabled;
    status->current_session_id = current_session_id;
    status->observed_frames = observed_frames;
    status->sampled_frames = sampled_frames;
    status->retained_records = retained_records;
    status->sample_suppressed_frames = sample_suppressed_frames;
    status->queue_dropped_records = queue_dropped_records;
    status->storage_write_failures = storage_write_failures;
    portEXIT_CRITICAL(&status_lock);
    if (!status->mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(file_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (current_file != NULL) {
        fflush(current_file);
    }
    status->current_bytes = file_size(VHOS_CAPTURE_CURRENT_PATH);
    status->previous_bytes = file_size(VHOS_CAPTURE_PREVIOUS_PATH);
    status->previous_session_id = session_from_file(VHOS_CAPTURE_PREVIOUS_PATH);
    xSemaphoreGive(file_lock);
    status->current_records = status->current_bytes >= VHOS_CAPTURE_HEADER_BYTES
        ? (status->current_bytes - VHOS_CAPTURE_HEADER_BYTES) / VHOS_CAPTURE_RECORD_BYTES
        : 0;
    status->previous_records = status->previous_bytes >= VHOS_CAPTURE_HEADER_BYTES
        ? (status->previous_bytes - VHOS_CAPTURE_HEADER_BYTES) / VHOS_CAPTURE_RECORD_BYTES
        : 0;
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        status->total_bytes = (uint32_t)total;
        status->free_bytes = total >= used ? (uint32_t)(total - used) : 0;
    }
    return ESP_OK;
}

esp_err_t vhos_capture_store_read_records(
    uint8_t slot,
    uint32_t record_offset,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length,
    uint32_t *record_count,
    bool *end_of_file
)
{
    if (output == NULL || output_length == NULL || record_count == NULL ||
        end_of_file == NULL || output_capacity < VHOS_CAPTURE_RECORD_BYTES ||
        (slot != VHOS_CAPTURE_SLOT_CURRENT && slot != VHOS_CAPTURE_SLOT_PREVIOUS)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = slot == VHOS_CAPTURE_SLOT_CURRENT
        ? VHOS_CAPTURE_CURRENT_PATH
        : VHOS_CAPTURE_PREVIOUS_PATH;
    if (xSemaphoreTake(file_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (slot == VHOS_CAPTURE_SLOT_CURRENT && current_file != NULL) {
        fflush(current_file);
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        xSemaphoreGive(file_lock);
        *output_length = 0;
        *record_count = 0;
        *end_of_file = true;
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t bytes = file_size(path);
    uint32_t available_records = bytes >= VHOS_CAPTURE_HEADER_BYTES
        ? (bytes - VHOS_CAPTURE_HEADER_BYTES) / VHOS_CAPTURE_RECORD_BYTES
        : 0;
    uint32_t remaining = record_offset < available_records
        ? available_records - record_offset
        : 0;
    uint32_t maximum = (uint32_t)(output_capacity / VHOS_CAPTURE_RECORD_BYTES);
    uint32_t requested = remaining < maximum ? remaining : maximum;
    bool seek_ok = fseek(
        file,
        (long)VHOS_CAPTURE_HEADER_BYTES + (long)record_offset * VHOS_CAPTURE_RECORD_BYTES,
        SEEK_SET
    ) == 0;
    size_t read = seek_ok
        ? fread(output, VHOS_CAPTURE_RECORD_BYTES, requested, file)
        : 0;
    fclose(file);
    xSemaphoreGive(file_lock);
    *record_count = (uint32_t)read;
    *output_length = read * VHOS_CAPTURE_RECORD_BYTES;
    *end_of_file = record_offset + read >= available_records;
    return seek_ok && read == requested ? ESP_OK : ESP_FAIL;
}

esp_err_t vhos_capture_store_rotate(void)
{
    if (!mounted || xSemaphoreTake(file_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = rotate_locked();
    xSemaphoreGive(file_lock);
    return result;
}

esp_err_t vhos_capture_store_set_logging(bool enabled)
{
    portENTER_CRITICAL(&status_lock);
    if (!mounted) {
        portEXIT_CRITICAL(&status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    logging_enabled = enabled;
    portEXIT_CRITICAL(&status_lock);
    if (!enabled && xSemaphoreTake(file_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (current_file != NULL) {
            fflush(current_file);
        }
        xSemaphoreGive(file_lock);
    }
    return ESP_OK;
}

uint32_t vhos_capture_store_current_session_id(void)
{
    uint32_t session_id;
    portENTER_CRITICAL(&status_lock);
    session_id = current_session_id;
    portEXIT_CRITICAL(&status_lock);
    return session_id;
}
