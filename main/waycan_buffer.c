// SPDX-License-Identifier: GPL-3.0-or-later
#include "waycan_buffer.h"
#include <string.h>

static void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 |
           (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

void waycan_buffer_init(waycan_buffer_t *buffer, uint32_t boot_id)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->boot_id = boot_id;
}

void waycan_buffer_append(waycan_buffer_t *buffer, uint32_t time_ms,
                          uint8_t pid, const uint8_t *data, uint8_t length)
{
    uint8_t *packet = buffer->packets[buffer->head];
    memset(packet, 0, WAYCAN_PACKET_SIZE);
    packet[0] = WAYCAN_VERSION;
    packet[1] = 1;
    put_u32(packet + 2, buffer->boot_id);
    put_u32(packet + 6, buffer->next++);
    put_u32(packet + 10, time_ms);
    packet[14] = pid;
    if (data != NULL && (length == 1 || length == 2)) {
        packet[15] = length;
        memcpy(packet + 16, data, length);
    }
    buffer->head = (buffer->head + 1) % WAYCAN_CAPACITY;
    if (buffer->count < WAYCAN_CAPACITY) buffer->count++;
}

void waycan_buffer_status(const waycan_buffer_t *buffer, uint32_t time_ms,
                          uint8_t packet[WAYCAN_PACKET_SIZE])
{
    memset(packet, 0, WAYCAN_PACKET_SIZE);
    packet[0] = WAYCAN_VERSION;
    put_u32(packet + 2, buffer->boot_id);
    put_u32(packet + 6, buffer->next - buffer->count);
    put_u32(packet + 10, buffer->next);
    put_u32(packet + 14, time_ms);
    packet[18] = WAYCAN_CAPACITY & 0xff;
    packet[19] = WAYCAN_CAPACITY >> 8;
}

bool waycan_buffer_resume(const waycan_buffer_t *buffer, const uint8_t *command,
                          size_t length, uint32_t *cursor)
{
    if (length != 10 || command[0] != WAYCAN_VERSION || command[1] != 1 ||
        get_u32(command + 2) != buffer->boot_id) return false;
    uint32_t requested = get_u32(command + 6);
    // Serial-number arithmetic: reject future cursors, including across wrap.
    if (buffer->next - requested > INT32_MAX) return false;
    *cursor = requested;
    return true;
}

bool waycan_buffer_peek(const waycan_buffer_t *buffer, uint32_t *cursor,
                        uint8_t packet[WAYCAN_PACKET_SIZE])
{
    uint32_t behind = buffer->next - *cursor;
    if (behind == 0 || buffer->count == 0 || behind > INT32_MAX) return false;
    if (behind > buffer->count) {
        behind = buffer->count;
        *cursor = buffer->next - behind;
    }
    size_t index = (buffer->head + WAYCAN_CAPACITY - behind) % WAYCAN_CAPACITY;
    memcpy(packet, buffer->packets[index], WAYCAN_PACKET_SIZE);
    return true;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool waycan_parse_pid(const char *response, size_t length, uint8_t pid,
                      uint8_t data[2])
{
    if (pid != 0x0d && pid != 0x0c && pid != 0x11) return false;
    uint8_t bytes[4] = {0};
    size_t digits = 0;
    for (size_t i = 0; i < length; i++) {
        char c = response[i];
        if (c == ' ' || c == '\r' || c == '\n') continue;
        int value = hex_digit(c);
        if (value < 0 || digits == sizeof(bytes) * 2) return false;
        bytes[digits / 2] = (uint8_t)((bytes[digits / 2] << 4) | value);
        digits++;
    }
    size_t expected = pid == 0x0c ? 8 : 6;
    if (digits != expected || bytes[0] != 0x41 || bytes[1] != pid) return false;
    data[0] = bytes[2];
    data[1] = bytes[3];
    return true;
}
