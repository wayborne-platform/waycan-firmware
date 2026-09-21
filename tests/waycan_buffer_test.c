// SPDX-License-Identifier: GPL-3.0-or-later
#include "waycan_buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void packets(void)
{
    waycan_buffer_t buffer;
    uint8_t packet[20], data[] = {0x1f, 0x40};
    uint32_t cursor = 0;
    waycan_buffer_init(&buffer, 0x12345678);
    assert(!waycan_buffer_peek(&buffer, &cursor, packet));
    waycan_buffer_append(&buffer, 1234, 0x0c, data, 2);
    assert(waycan_buffer_peek(&buffer, &cursor, packet));
    const uint8_t expected[] = {1,1,0x78,0x56,0x34,0x12,0,0,0,0,0xd2,4,0,0,0x0c,2,0x1f,0x40,0,0};
    assert(memcmp(packet, expected, sizeof(expected)) == 0);
    assert(cursor == 0); // A failed send can retry the same record.
    cursor++;
    assert(!waycan_buffer_peek(&buffer, &cursor, packet));
    waycan_buffer_append(&buffer, 1300, 0x11, data, 0);
    assert(waycan_buffer_peek(&buffer, &cursor, packet));
    assert(packet[15] == 0 && packet[16] == 0 && packet[17] == 0);
    waycan_buffer_status(&buffer, 1400, packet);
    assert(packet[0] == 1 && packet[1] == 0);
    assert(u32(packet + 2) == 0x12345678 && u32(packet + 6) == 0);
    assert(u32(packet + 10) == 2 && u32(packet + 14) == 1400);
    assert(packet[18] == WAYCAN_CAPACITY && packet[19] == 0);
}

static void replay_and_overflow(void)
{
    waycan_buffer_t buffer;
    uint8_t packet[20], data[] = {40};
    waycan_buffer_init(&buffer, 9);
    for (unsigned i = 0; i < WAYCAN_CAPACITY + 7; i++)
        waycan_buffer_append(&buffer, i * 333, 0x0d, data, 1);
    uint32_t cursor = 0;
    assert(waycan_buffer_peek(&buffer, &cursor, packet));
    assert(cursor == 7 && u32(packet + 6) == 7);
    for (unsigned i = 7; i < WAYCAN_CAPACITY + 7; i++) {
        assert(waycan_buffer_peek(&buffer, &cursor, packet));
        assert(u32(packet + 6) == i && u32(packet + 10) == i * 333);
        cursor++;
    }
    assert(!waycan_buffer_peek(&buffer, &cursor, packet));
    uint8_t resume[] = {1,1,9,0,0,0,100,0,0,0};
    assert(waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
    assert(cursor == 100);
    assert(waycan_buffer_peek(&buffer, &cursor, packet) && u32(packet + 6) == 100);
    for (size_t n = 0; n < sizeof(resume); n++)
        assert(!waycan_buffer_resume(&buffer, resume, n, &cursor));
    resume[2] = 10;
    assert(!waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
    resume[2] = 9;
    resume[6] = 250;
    assert(!waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
    assert(cursor == 100);
    resume[6] = 100;
    resume[0] = 2;
    assert(!waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
    resume[0] = 1;
    resume[1] = 2;
    assert(!waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
}

static void wraparound(void)
{
    waycan_buffer_t buffer;
    uint8_t packet[20], data[] = {0};
    waycan_buffer_init(&buffer, 1);
    buffer.next = UINT32_MAX - 1;
    for (unsigned i = 0; i < 4; i++) waycan_buffer_append(&buffer, UINT32_MAX - 1 + i, 0x0d, data, 1);
    uint32_t cursor = UINT32_MAX - 1;
    for (unsigned i = 0; i < 4; i++) {
        assert(waycan_buffer_peek(&buffer, &cursor, packet));
        assert(u32(packet + 6) == cursor && u32(packet + 10) == cursor);
        cursor++;
    }
    assert(!waycan_buffer_peek(&buffer, &cursor, packet));
    const uint8_t resume[] = {1,1,1,0,0,0,0xff,0xff,0xff,0xff};
    assert(waycan_buffer_resume(&buffer, resume, sizeof(resume), &cursor));
    assert(waycan_buffer_peek(&buffer, &cursor, packet) && u32(packet + 6) == UINT32_MAX);
}

static void parsing(void)
{
    uint8_t data[2] = {0};
    assert(waycan_parse_pid("410D3C\r", 7, 0x0d, data) && data[0] == 60);
    assert(waycan_parse_pid("41 0c 1f 40\r", 12, 0x0c, data));
    assert(data[0] == 0x1f && data[1] == 0x40);
    assert(waycan_parse_pid("4111FF\r", 7, 0x11, data) && data[0] == 255);
    const char *invalid[] = {"NO DATA\r\r>", "410D\r", "410D1\r", "410DXY\r",
        "410DFFAA\r", "410C1234\r", "7F0112\r", "7E803410DFF\r", "\r>", ""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!waycan_parse_pid(invalid[i], strlen(invalid[i]), 0x0d, data));
    // Arbitrary BLE/ELM byte sequences must stay bounded under sanitizers.
    uint32_t seed = 13;
    for (unsigned i = 0; i < 10000; i++) {
        char bytes[128];
        for (unsigned j = 0; j < sizeof(bytes); j++) {
            seed = seed * 1664525u + 1013904223u;
            bytes[j] = (char)(seed >> 24);
        }
        waycan_parse_pid(bytes, i % sizeof(bytes), 0x0c, data);
    }
}

int main(void)
{
    packets();
    replay_and_overflow();
    wraparound();
    parsing();
    puts("waycan buffer: packet encoding, replay, overflow, wrap, and parsing passed");
}
