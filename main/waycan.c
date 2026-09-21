// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdkconfig.h"
#include "waycan.h"
#include "config_server.h"

static bool enabled;

bool waycan_enabled(void)
{
    return enabled;
}

#ifdef CONFIG_WAYCAN_TELEMETRY
#if HARDWARE_VER != WICAN_V300
#error "WayCAN telemetry currently targets WiCAN-OBD hardware v3.00 only"
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "elm327.h"
#include "can.h"
#include "dev_status.h"
#include "sleep_mode.h"

static waycan_buffer_t history;
static SemaphoreHandle_t mutex;
static bool subscribed;
static bool streaming;
static uint32_t cursor;
// The sampler owns ELM327 in this mode. Its response callback is synchronous.
static uint8_t pending_pid;
static uint8_t response_data[2];
static bool response_valid;
static uint32_t response_time;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void waycan_response(char *response, uint32_t length, QueueHandle_t *queue)
{
    (void)queue;
    if (!response_valid && waycan_parse_pid(response,
            length ? length : strlen(response), pending_pid, response_data)) {
        response_valid = true;
        response_time = now_ms();
    }
}

static void command(const char *text)
{
    twai_message_t frame = {0};
    elm327_process_cmd((uint8_t *)text, strlen(text), &frame, NULL);
}

static bool can_poll(void)
{
    float voltage = 0;
    float threshold = 0;
    return dev_status_is_awake() && can_is_enabled() && config_server_get_ble_config() == 1 &&
           config_server_get_sleep_config() == 1 &&
           config_server_mqtt_en_config() == 0 &&
           config_server_get_sleep_volt(&threshold) != -1 &&
           sleep_mode_get_voltage(&voltage) == 1 &&
           isfinite(voltage) && voltage > threshold;
}

static void sampler(void *unused)
{
    (void)unused;
    const uint8_t pids[] = {0x0d, 0x0c, 0x11};
    char request[12];
    elm327_lock();
    command("ATZ\r");
    command("ATE0\rATL0\rATS0\rATH0\rATST32\r");
    snprintf(request, sizeof(request), "ATSP%d\r", CONFIG_WAYCAN_OBD_PROTOCOL);
    command(request);
    elm327_unlock();
    while (true) {
        int64_t started = esp_timer_get_time();
        for (size_t i = 0; i < sizeof(pids); i++) {
            if (!can_poll()) break;
            pending_pid = pids[i];
            response_valid = false;
            memset(response_data, 0, sizeof(response_data));
            // One reply is sufficient; avoid waiting for other ECU responders.
            snprintf(request, sizeof(request), "01%02X1\r", pending_pid);
            elm327_lock();
            command(request);
            elm327_unlock();
            xSemaphoreTake(mutex, portMAX_DELAY);
            waycan_buffer_append(&history, response_valid ? response_time : now_ms(),
                pending_pid, response_data, response_valid ? (pending_pid == 0x0c ? 2 : 1) : 0);
            xSemaphoreGive(mutex);
            pending_pid = 0;
        }
        // Never catch up with a burst after a delayed cycle.
        int64_t remaining = 1000 - (esp_timer_get_time() - started) / 1000;
        vTaskDelay(pdMS_TO_TICKS(remaining > 20 ? remaining : 20));
    }
}

void waycan_init(void)
{
    enabled = config_server_protocol() == OBD_ELM327;
    if (!enabled) return;
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex != NULL);
    waycan_buffer_init(&history, esp_random());
}

void waycan_start(void)
{
    if (!enabled) return;
    BaseType_t result = xTaskCreate(sampler, "waycan", 4096, NULL, 5, NULL);
    configASSERT(result == pdPASS);
}

bool waycan_status(uint8_t packet[WAYCAN_PACKET_SIZE])
{
    if (mutex == NULL) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    waycan_buffer_status(&history, now_ms(), packet);
    xSemaphoreGive(mutex);
    return true;
}

bool waycan_resume(const uint8_t *value, size_t length)
{
    if (mutex == NULL) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool valid = subscribed && waycan_buffer_resume(&history, value, length, &cursor);
    if (valid) streaming = true;
    xSemaphoreGive(mutex);
    return valid;
}

void waycan_subscribe(bool enabled)
{
    if (mutex == NULL) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    subscribed = enabled;
    streaming = false;
    xSemaphoreGive(mutex);
}

void waycan_pump(bool (*send)(uint8_t *packet))
{
    if (mutex == NULL) return;
    uint8_t packet[WAYCAN_PACKET_SIZE];
    // Serialize disconnect/resume with submission so an old send cannot advance a new cursor.
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (streaming && waycan_buffer_peek(&history, &cursor, packet) && send(packet)) cursor++;
    xSemaphoreGive(mutex);
}
#else
void waycan_init(void) {}
void waycan_start(void) {}
void waycan_response(char *response, uint32_t length, QueueHandle_t *queue)
{ (void)response; (void)length; (void)queue; }
bool waycan_status(uint8_t packet[WAYCAN_PACKET_SIZE]) { (void)packet; return false; }
bool waycan_resume(const uint8_t *value, size_t length) { (void)value; (void)length; return false; }
void waycan_subscribe(bool enabled) { (void)enabled; }
void waycan_pump(bool (*send)(uint8_t *packet)) { (void)send; }
#endif
