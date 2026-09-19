#include "camera_app/private_uart.h"

#include "camera_app/siyi.h"

#include <string.h>

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void put_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

uint8_t ca_crc8_maxim(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    size_t i;
    unsigned bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) != 0U ? (uint8_t)((crc >> 1) ^ 0x8cU)
                                   : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

size_t ca_private_build(uint8_t *output, size_t capacity, uint8_t control,
                        uint16_t sequence, uint8_t source, uint8_t destination,
                        uint8_t link, uint8_t command, const uint8_t *payload,
                        uint16_t payload_length)
{
    size_t total = (size_t)payload_length + 14U;

    if (output == NULL || capacity < total || payload_length > CA_PRIVATE_MAX_PAYLOAD) return 0;
    output[0] = 0xaa;
    output[1] = control;
    output[2] = 3;
    put_u16_le(output + 3, payload_length);
    output[5] = ca_crc8_maxim(output, 5);
    put_u16_le(output + 6, sequence);
    output[8] = source;
    output[9] = destination;
    output[10] = link;
    output[11] = command;
    if (payload_length != 0U && payload != NULL) memcpy(output + 12, payload, payload_length);
    put_u16_le(output + total - 2U, ca_crc16(output, total - 2U));
    return total;
}

void ca_private_parser_init(struct ca_private_parser *parser)
{
    memset(parser, 0, sizeof(*parser));
}

static void discard_prefix(struct ca_private_parser *parser, size_t count)
{
    if (count >= parser->length) {
        parser->length = 0;
        return;
    }
    memmove(parser->data, parser->data + count, parser->length - count);
    parser->length -= count;
}

void ca_private_parser_feed(struct ca_private_parser *parser,
                            const uint8_t *data, size_t length,
                            ca_private_frame_fn callback, void *opaque)
{
    size_t copy;

    while (length != 0U) {
        copy = sizeof(parser->data) - parser->length;
        if (copy > length) copy = length;
        if (copy == 0U) {
            parser->discarded++;
            discard_prefix(parser, 1);
            continue;
        }
        memcpy(parser->data + parser->length, data, copy);
        parser->length += copy;
        data += copy;
        length -= copy;

        while (parser->length != 0U) {
            size_t sync = 0;
            uint16_t payload_length;
            size_t total;
            uint16_t received_crc;
            struct ca_private_frame frame;

            while (sync < parser->length && parser->data[sync] != 0xaaU) sync++;
            if (sync != 0U) {
                parser->discarded += (unsigned)sync;
                discard_prefix(parser, sync);
            }
            if (parser->length < 6U) break;
            payload_length = get_u16_le(parser->data + 3);
            if (parser->data[2] != 3U || payload_length > CA_PRIVATE_MAX_PAYLOAD) {
                parser->discarded++;
                discard_prefix(parser, 1);
                continue;
            }
            total = (size_t)payload_length + 14U;
            if (parser->length < total) break;
            if (parser->data[5] != ca_crc8_maxim(parser->data, 5)) {
                parser->bad_header_crc++;
                discard_prefix(parser, 1);
                continue;
            }
            received_crc = get_u16_le(parser->data + total - 2U);
            if (received_crc != ca_crc16(parser->data, total - 2U)) {
                parser->bad_frame_crc++;
                discard_prefix(parser, 1);
                continue;
            }
            frame.control = parser->data[1];
            frame.sequence = get_u16_le(parser->data + 6);
            frame.source = parser->data[8];
            frame.destination = parser->data[9];
            frame.link = parser->data[10];
            frame.command = parser->data[11];
            frame.payload = parser->data + 12;
            frame.payload_length = payload_length;
            if (callback != NULL) callback(opaque, &frame);
            discard_prefix(parser, total);
        }
    }
}
