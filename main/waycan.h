// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYCAN_H
#define WAYCAN_H

#include "waycan_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

bool waycan_enabled(void);
void waycan_init(void);
void waycan_start(void);
void waycan_response(char *response, uint32_t length, QueueHandle_t *queue);
bool waycan_status(uint8_t packet[WAYCAN_PACKET_SIZE]);
bool waycan_resume(const uint8_t *command, size_t length);
void waycan_subscribe(bool enabled);
void waycan_pump(bool (*send)(uint8_t *packet));

#endif
