// SPDX-License-Identifier: GPL-3.0-or-later
#include "waycan.h"
#include "config_server.h"
#include "dev_status.h"
#include "driver/twai.h"
#include "elm327.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*sampler_task)(void *);
static jmp_buf cycle_done;
static int64_t clock_us;
static int8_t protocol = SLCAN, ble = 1, sleep_enabled = 1, mqtt;
static float voltage = 14.0f;
static bool awake = true, voltage_available = true, elm_locked, can_enabled = true;
static bool drop_voltage, fail_send, fail_rpm;
static unsigned requests, sent;
static uint8_t last_packet[20];

int8_t config_server_protocol(void) { return protocol; }
int8_t config_server_get_ble_config(void) { return ble; }
int8_t config_server_get_sleep_config(void) { return sleep_enabled; }
int8_t config_server_mqtt_en_config(void) { return mqtt; }
int8_t config_server_get_sleep_volt(float *value) { *value = 13.1f; return 1; }
int8_t sleep_mode_get_voltage(float *value) { *value = voltage; return voltage_available ? 1 : -1; }
bool dev_status_is_bit_set(EventBits_t bits) { assert(bits == DEV_AWAKE_BIT); return awake; }
bool can_is_enabled(void) { return can_enabled; }
int64_t esp_timer_get_time(void) { return clock_us; }
uint32_t esp_random(void) { return 9; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &history_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{ (void)timeout; assert(pthread_mutex_lock(mutex) == 0); return pdPASS; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(pthread_mutex_unlock(mutex) == 0); return pdPASS; }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    sampler_task = task;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { clock_us += ticks * 1000; longjmp(cycle_done, 1); }
void elm327_lock(void) { assert(!elm_locked); elm_locked = true; }
void elm327_unlock(void) { assert(elm_locked); elm_locked = false; }
int8_t elm327_process_cmd(uint8_t *buf, uint8_t len, twai_message_t *frame, QueueHandle_t *q)
{
    (void)frame;
    assert(elm_locked && q == NULL && len == strlen((char *)buf));
    if (buf[0] == 'A') return 0;
    requests++;
    clock_us += 17000;
    if (strcmp((char *)buf, "010D1\r") == 0) waycan_response("410D3C\r", 0, q);
    else if (strcmp((char *)buf, "010C1\r") == 0)
        waycan_response(fail_rpm ? "NO DATA\r\r>" : "410C1F40\r", 0, q);
    else {
        assert(strcmp((char *)buf, "01111\r") == 0);
        waycan_response("411180\r", 0, q);
    }
    waycan_response("\r>", 0, q);
    if (drop_voltage) voltage = 12.0f;
    return 0;
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void cycle(void)
{
    assert(sampler_task != NULL);
    if (setjmp(cycle_done) == 0) sampler_task(NULL);
}
static bool send_packet(uint8_t *packet)
{
    memcpy(last_packet, packet, 20);
    if (fail_send) return false;
    sent++;
    return true;
}
static void resume(uint8_t sequence)
{
    uint8_t command[] = {1,1,9,0,0,0,sequence,0,0,0};
    assert(waycan_resume(command, sizeof(command)));
}

int main(void)
{
    waycan_init();
    assert(!waycan_enabled());
    waycan_start();
    assert(sampler_task == NULL);
    protocol = OBD_ELM327;
    waycan_init();
    waycan_start();
    protocol = SLCAN;
    assert(waycan_enabled()); // Runtime settings cannot change channel ownership.
    protocol = OBD_ELM327;

    cycle(); // No phone subscribed; still collect three samples.
    assert(requests == 3);
    uint8_t status[20];
    assert(waycan_status(status) && u32(status + 10) == 3);
    const uint8_t command[] = {1,1,9,0,0,0,0,0,0,0};
    assert(!waycan_resume(command, sizeof(command)));
    waycan_pump(send_packet);
    assert(sent == 0);
    waycan_subscribe(true);
    waycan_pump(send_packet);
    assert(sent == 0); // CCC alone never replays somebody else's cursor.
    resume(0);
    fail_send = true;
    waycan_pump(send_packet);
    assert(sent == 0 && u32(last_packet + 6) == 0);
    fail_send = false;
    for (unsigned i = 0; i < 3; i++) {
        waycan_pump(send_packet);
        assert(u32(last_packet + 6) == i);
        assert(u32(last_packet + 10) == (i + 1) * 17);
    }
    assert(sent == 3);
    waycan_pump(send_packet);
    assert(sent == 3);

    waycan_subscribe(false);
    fail_rpm = true;
    cycle();
    waycan_pump(send_packet);
    assert(requests == 6 && sent == 3);
    waycan_subscribe(true);
    resume(3);
    waycan_pump(send_packet);
    assert(u32(last_packet + 6) == 3 && last_packet[15] == 1);
    waycan_pump(send_packet);
    assert(u32(last_packet + 6) == 4 && last_packet[14] == 0x0c);
    assert(last_packet[15] == 0 && last_packet[16] == 0 && last_packet[17] == 0);

    unsigned before = requests;
    voltage = 12.0f; cycle(); assert(requests == before);
    voltage = 13.1f; cycle(); assert(requests == before);
    voltage = NAN; cycle(); assert(requests == before);
    voltage = 14.0f;
    voltage_available = false; cycle(); assert(requests == before);
    voltage_available = true;
    awake = false; cycle(); assert(requests == before);
    awake = true;
    can_enabled = false; cycle(); assert(requests == before);
    can_enabled = true;
    sleep_enabled = 0; cycle(); assert(requests == before);
    sleep_enabled = 1;
    mqtt = 1; cycle(); assert(requests == before);
    mqtt = 0;
    ble = 0; cycle(); assert(requests == before);
    ble = 1;
    drop_voltage = true; cycle(); assert(requests == before + 1);
    drop_voltage = false;
    voltage = 14.0f; cycle(); assert(requests == before + 4);
    puts("waycan runtime: disconnected sampling, timestamps, replay, failed sends, invalid data, and voltage gating passed");
}
