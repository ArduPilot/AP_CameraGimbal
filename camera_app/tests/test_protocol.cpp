#include "camera_app/private_uart.h"
#include "camera_app/siyi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct captured {
    unsigned count;
    uint8_t opcode;
    uint16_t private_sequence;
};

static void capture_frame(void *opaque, const struct ca_private_frame *frame)
{
    struct captured *captured = (struct captured*)(opaque);
    struct ca_siyi_packet packet;
    size_t consumed;

    assert(frame->source == 0x34);
    assert(frame->destination == 0x2e);
    assert(frame->link == 0x6b);
    assert(frame->command == 0x16);
    assert(ca_siyi_parse_one(frame->payload, frame->payload_length,
                             &packet, &consumed) == 1);
    assert(consumed == frame->payload_length);
    captured->count++;
    captured->opcode = packet.opcode;
    captured->private_sequence = frame->sequence;
}

int main(void)
{
    uint8_t public_packet[64];
    uint8_t private_frame[128];
    uint8_t payload[] = {0xd4, 0xfe, 0xd2, 0xfe};
    size_t public_length;
    size_t private_length;
    struct ca_siyi_packet parsed;
    size_t consumed;
    struct ca_private_parser parser;
    struct captured captured = {};

    assert(ca_crc16((const uint8_t *)"123456789", 9) == 0x31c3);
    public_length = ca_siyi_build(public_packet, sizeof(public_packet), 1,
                                  3, 0x0e, payload, sizeof(payload));
    assert(public_length == 14);
    assert(ca_siyi_parse_one(public_packet, public_length, &parsed, &consumed) == 1);
    assert(consumed == public_length);
    assert(parsed.opcode == 0x0e && parsed.sequence == 3);
    assert(parsed.payload_length == sizeof(payload));
    assert(memcmp(parsed.payload, payload, sizeof(payload)) == 0);

    private_length = ca_private_build(private_frame, sizeof(private_frame),
                                      0x08, 42, 0x34, 0x2e, 0x6b, 0x16,
                                      public_packet, (uint16_t)public_length);
    assert(private_length == public_length + 14);
    ca_private_parser_init(&parser);
    ca_private_parser_feed(&parser, private_frame, 7, capture_frame, &captured);
    assert(captured.count == 0);
    ca_private_parser_feed(&parser, private_frame + 7, private_length - 7,
                           capture_frame, &captured);
    assert(captured.count == 1);
    assert(captured.opcode == 0x0e && captured.private_sequence == 42);
    assert(parser.bad_header_crc == 0 && parser.bad_frame_crc == 0);

    {
        static const uint8_t captured_public[] = {
            0x55, 0x66, 0x01, 0x00, 0x00, 0x03, 0x00, 0x0d, 0xb8, 0x5c};
        static const uint8_t captured_private[] = {
            0xaa, 0x08, 0x03, 0x0a, 0x00, 0x91, 0xb1, 0x00,
            0x34, 0x2e, 0x6b, 0x16, 0x55, 0x66, 0x01, 0x00,
            0x00, 0x03, 0x00, 0x0d, 0xb8, 0x5c, 0x0f, 0xa4};
        assert(ca_siyi_build(public_packet, sizeof(public_packet), 1, 3,
                             0x0d, NULL, 0) == sizeof(captured_public));
        assert(memcmp(public_packet, captured_public, sizeof(captured_public)) == 0);
        assert(ca_private_build(private_frame, sizeof(private_frame), 0x08,
                                177, 0x34, 0x2e, 0x6b, 0x16,
                                public_packet, sizeof(captured_public)) ==
               sizeof(captured_private));
        assert(memcmp(private_frame, captured_private, sizeof(captured_private)) == 0);
    }

    public_length = ca_siyi_build(public_packet, sizeof(public_packet), 2,
                                  99, 0x0a,
                                  (const uint8_t *)"\0\0\0\0\0\1\0\0", 8);
    assert(ca_siyi_rewrite_payload_byte(public_packet, public_length, 3, 1));
    assert(ca_siyi_parse_one(public_packet, public_length, &parsed, &consumed) == 1);
    assert(parsed.payload[3] == 1);
    assert(ca_siyi_opcode_known(0x0e));
    assert(ca_siyi_opcode_known(0x19));
    assert(ca_siyi_opcode_known(0x22));
    assert(ca_siyi_opcode_known(0x17));
    assert(!ca_siyi_opcode_known(0x7f));
    puts("PASS protocol framing and stream parser");
    return 0;
}
