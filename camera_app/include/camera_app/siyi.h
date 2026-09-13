#ifndef CAMERA_APP_SIYI_H
#define CAMERA_APP_SIYI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CA_SIYI_MIN_PACKET 10U
#define CA_SIYI_MAX_PACKET 2048U

struct ca_siyi_packet {
    uint8_t control;
    uint16_t payload_length;
    uint16_t sequence;
    uint8_t opcode;
    const uint8_t *payload;
    const uint8_t *raw;
    size_t raw_length;
};

uint16_t ca_crc16(const uint8_t *data, size_t length);
int ca_siyi_parse_one(const uint8_t *data, size_t length,
                      struct ca_siyi_packet *packet, size_t *consumed);
size_t ca_siyi_build(uint8_t *output, size_t capacity, uint8_t control,
                     uint16_t sequence, uint8_t opcode,
                     const uint8_t *payload, uint16_t payload_length);
bool ca_siyi_rewrite_payload_byte(uint8_t *packet, size_t length,
                                  size_t payload_offset, uint8_t value);
bool ca_siyi_opcode_known(uint8_t opcode);

#endif
