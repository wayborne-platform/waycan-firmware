// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYCAN_BUFFER_H
#define WAYCAN_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAYCAN_PACKET_SIZE 20
#define WAYCAN_CAPACITY 192
#define WAYCAN_VERSION 1

typedef struct {
    uint8_t packets[WAYCAN_CAPACITY][WAYCAN_PACKET_SIZE];
    uint32_t boot_id;
    uint32_t next;
    uint16_t count;
    uint16_t head;
} waycan_buffer_t;

void waycan_buffer_init(waycan_buffer_t *buffer, uint32_t boot_id);
void waycan_buffer_append(waycan_buffer_t *buffer, uint32_t time_ms,
                          uint8_t pid, const uint8_t *data, uint8_t length);
void waycan_buffer_status(const waycan_buffer_t *buffer, uint32_t time_ms,
                          uint8_t packet[WAYCAN_PACKET_SIZE]);
bool waycan_buffer_resume(const waycan_buffer_t *buffer, const uint8_t *command,
                          size_t length, uint32_t *cursor);
// Clamps an overwritten cursor to the oldest retained record. Does not consume it.
bool waycan_buffer_peek(const waycan_buffer_t *buffer, uint32_t *cursor,
                        uint8_t packet[WAYCAN_PACKET_SIZE]);
bool waycan_parse_pid(const char *response, size_t length, uint8_t pid,
                      uint8_t data[2]);

#endif
