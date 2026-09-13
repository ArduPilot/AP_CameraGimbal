#!/usr/bin/env python3
"""Exercise concurrent UDP/TCP-to-private-UART camera protocol bridging."""

import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time


def crc16(data):
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value >> 1) ^ 0x8C) if value & 1 else value >> 1
    return value


def siyi(control, sequence, opcode, payload=b""):
    packet = struct.pack("<BBBHHB", 0x55, 0x66, control, len(payload), sequence, opcode) + payload
    return packet + struct.pack("<H", crc16(packet))


def private(control, sequence, source, destination, command, payload=b""):
    header = bytearray(struct.pack("<BBBHBHBBBB", 0xAA, control, 3, len(payload), 0,
                                   sequence, source, destination, 0x6B, command))
    header[5] = crc8(header[:5])
    frame = bytes(header) + payload
    return frame + struct.pack("<H", crc16(frame))


def read_frames(master, count, timeout=2.0):
    data = bytearray()
    frames = []
    deadline = time.monotonic() + timeout
    while len(frames) < count and time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], deadline - time.monotonic())
        if not readable:
            break
        data.extend(os.read(master, 4096))
        while len(data) >= 5:
            if data[0] != 0xAA:
                del data[0]
                continue
            total = 14 + struct.unpack_from("<H", data, 3)[0]
            if len(data) < total:
                break
            frames.append(bytes(data[:total]))
            del data[:total]
    return frames


def read_siyi_frame(master, timeout=2.0):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], deadline - time.monotonic())
        if not readable:
            break
        data.extend(os.read(master, 4096))
        while len(data) >= 2 and data[:2] != b"\x55\x66":
            del data[0]
        if len(data) >= 10:
            total = 10 + struct.unpack_from("<H", data, 3)[0]
            if len(data) >= total:
                return bytes(data[:total])
    raise TimeoutError("SIYI UART reply")


def reserve_udp_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    return sock, port


class SiyiStream:
    """Read complete SIYI packets from a TCP byte stream."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = bytearray()

    def recv(self):
        deadline = time.monotonic() + self.sock.gettimeout()
        while True:
            if len(self.buffer) >= 10:
                assert self.buffer[:2] == b"\x55\x66"
                total = 10 + struct.unpack_from("<H", self.buffer, 3)[0]
                if len(self.buffer) >= total:
                    packet = bytes(self.buffer[:total])
                    del self.buffer[:total]
                    return packet
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout()
            self.sock.settimeout(remaining)
            self.buffer.extend(self.sock.recv(4096))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_integration.py CAMERA_APP_HOST")
    master, slave = pty.openpty()
    slave_path = os.ttyname(slave)
    external_master, external_slave = pty.openpty()
    external_slave_path = os.ttyname(external_slave)
    blocker, port = reserve_udp_port()
    with tempfile.TemporaryDirectory() as directory:
        captured_stderr = ""
        ready = os.path.join(directory, "ready")
        record = os.path.join(directory, "recording")
        config = os.path.join(directory, "camera.ini")
        with open(config, "w", encoding="ascii") as config_file:
            config_file.write(
                "[general]\n"
                "timezone = UTC\n"
                "[capture]\n"
                "photo_scope = thermal\n"
                "[uart]\n"
                "protocol = siyi\n")
        env = os.environ.copy()
        env["CAMERA_APP_READY_PATH"] = ready
        env["CAMERA_APP_RECORD_STATE"] = record
        env["CAMERA_APP_TEST_SETTIME_FAILURE"] = "1"
        env["CAMERA_APP_MAVLINK_TCP_PORT"] = "0"
        env["CAMERA_APP_MAVLINK_UDP_PORT"] = "0"
        env["CAMERA_APP_EXTERNAL_UART"] = external_slave_path
        process = subprocess.Popen(
            [sys.argv[1], "--uart", slave_path, "--port", str(port),
             "--config", config],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(0.25)
            assert process.poll() is None
            assert not os.path.exists(ready)
            blocker.close()
            deadline = time.monotonic() + 3
            while not os.path.exists(ready) and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            assert os.path.exists(ready), process.stderr.read()
            serial_settings = termios.tcgetattr(external_slave)
            assert serial_settings[4] == termios.B230400
            assert serial_settings[5] == termios.B230400
            control = serial_settings[2]
            assert control & termios.CSIZE == termios.CS8
            assert not control & termios.PARENB
            assert not control & termios.CSTOPB
            startup = read_frames(master, 5)
            assert len(startup) == 5
            assert [(frame[1], frame[11]) for frame in startup] == [
                (0x09, 0x13), (0x09, 0x17), (0x09, 0xC7), (0x09, 0x62), (0x08, 0x0D)]

            os.write(external_master, siyi(1, 38, 0x16))
            serial_reply = read_siyi_frame(external_master)
            assert serial_reply[7] == 0x16 and serial_reply[8:10] == b"\x0a\x00"
            os.close(external_master)
            external_master = -1

            # Losing the optional UART must not take down the network service.
            tcp1 = socket.create_connection(("127.0.0.1", port), timeout=1)
            tcp2 = socket.create_connection(("127.0.0.1", port), timeout=1)
            stream1 = SiyiStream(tcp1)
            stream2 = SiyiStream(tcp2)

            tcp1.sendall(siyi(1, 39, 0x00, b"\x00"))
            assert read_frames(master, 1, timeout=0.1) == []

            fragmented = siyi(1, 40, 0x16)
            tcp1.sendall(fragmented[:5])
            tcp1.settimeout(0.05)
            try:
                tcp1.recv(1)
                raise AssertionError("partial TCP request produced a reply")
            except socket.timeout:
                pass
            tcp1.settimeout(1)
            tcp1.sendall(fragmented[5:])
            range_reply = stream1.recv()
            assert range_reply[7] == 0x16 and range_reply[8:10] == b"\x0a\x00"

            tcp2.sendall(siyi(1, 41, 0x16) + siyi(1, 42, 0x18))
            replies = [stream2.recv(), stream2.recv()]
            assert [reply[7] for reply in replies] == [0x16, 0x18]

            request1 = siyi(1, 43, 0x0D)
            request2 = siyi(1, 44, 0x0D)
            tcp1.sendall(request1)
            tunneled = read_frames(master, 1)
            assert len(tunneled) == 1 and tunneled[0][12:-2] == request1
            tcp2.sendall(request2)
            tunneled = read_frames(master, 1)
            assert len(tunneled) == 1 and tunneled[0][12:-2] == request2

            reply1 = siyi(2, 140, 0x0D,
                          struct.pack("<hhhhhh", -100, -300, 0, 0, 0, 0))
            reply2 = siyi(2, 141, 0x0D,
                          struct.pack("<hhhhhh", -200, -400, 0, 0, 0, 0))
            os.write(master, private(0x0A, 50, 0x2E, 0x34, 0x16, reply1))
            os.write(master, private(0x0A, 51, 0x2E, 0x34, 0x16, reply2))
            assert stream1.recv() == reply1
            assert stream2.recv() == reply2
            tcp1.close()
            tcp2.close()

            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(1)
            client.connect(("127.0.0.1", port))
            client.send(siyi(1, 6, 0x04,
                             struct.pack("<BHH", 1, 960, 540)))
            autofocus_reply = client.recv(2048)
            assert autofocus_reply[7] == 0x04
            assert struct.unpack_from("<H", autofocus_reply, 3)[0] == 1
            assert autofocus_reply[8] == 1
            client.send(siyi(1, 37, 0x06, struct.pack("<b", -1)))
            focus_reply = client.recv(2048)
            assert focus_reply[7] == 0x06 and focus_reply[8] == 1
            client.send(siyi(1, 38, 0x06, struct.pack("<b", 0)))
            focus_reply = client.recv(2048)
            assert focus_reply[7] == 0x06 and focus_reply[8] == 1
            request = siyi(1, 7, 0x0D)
            client.send(request)
            tunneled = read_frames(master, 1)
            assert len(tunneled) == 1 and tunneled[0][11] == 0x16
            assert tunneled[0][12:-2] == request

            attitude = struct.pack("<hhhhhh", -100, -300, 0, 0, 0, 0)
            reply = siyi(2, 100, 0x0D, attitude)
            os.write(master, private(0x0A, 50, 0x2E, 0x34, 0x16, reply))
            assert client.recv(2048) == reply

            target_request = siyi(1, 51, 0x17)
            client.send(target_request)
            tunneled = read_frames(master, 1)
            assert len(tunneled) == 1 and tunneled[0][12:-2] == target_request
            target_reply = siyi(
                2, 101, 0x17, struct.pack("<ii", 1491234567, -351234567))
            os.write(master, private(
                0x0A, 52, 0x2E, 0x34, 0x16, target_reply))
            assert client.recv(2048) == target_reply

            client.send(siyi(1, 8, 0x0C, b"\x02"))
            feedback = client.recv(2048)
            assert feedback[7] == 0x0B and feedback[8] == 5
            deadline = time.monotonic() + 1
            while not os.path.exists(record) and time.monotonic() < deadline:
                time.sleep(0.01)
            assert os.path.exists(record)
            client.send(siyi(1, 9, 0x0A))
            status_reply = client.recv(2048)
            assert status_reply[7] == 0x0A and status_reply[11] == 1
            client.send(siyi(1, 10, 0x0C, b"\x02"))
            feedback = client.recv(2048)
            assert feedback[7] == 0x0B and feedback[8] == 6
            time.sleep(0.05)
            assert not os.path.exists(record)
            client.send(siyi(1, 11, 0x0A))
            status_reply = client.recv(2048)
            assert status_reply[7] == 0x0A and status_reply[11] == 0
            client.send(siyi(1, 45, 0x0B))
            feedback = client.recv(2048)
            assert feedback[7] == 0x0B and feedback[8] == 6
            client.send(siyi(1, 46, 0x0C, b"\x01"))
            feedback = client.recv(2048)
            assert feedback[7] == 0x0B and feedback[8] == 3
            client.send(siyi(1, 47, 0x0A))
            status_reply = client.recv(2048)
            assert status_reply[7] == 0x0A and status_reply[9] == 0
            client.send(siyi(1, 48, 0x0C, b"\x01"))
            feedback = client.recv(2048)
            assert feedback[7] == 0x0B and feedback[8] == 3
            client.send(siyi(1, 13, 0x30, struct.pack("<Q", 1_700_000_000_123_456)))
            settime_reply = client.recv(2048)
            assert settime_reply[7] == 0x30 and settime_reply[8] == 0
            client.send(siyi(1, 15, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x01\x02"
            client.send(siyi(1, 49, 0x11, b"\x02\x00"))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x11 and slots_reply[8:10] == b"\x02\x00"
            client.send(siyi(1, 50, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x02\x00"
            client.send(siyi(1, 21, 0x11, b"\x00\x02"))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x11 and slots_reply[8:10] == b"\x00\x02"
            client.send(siyi(1, 22, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x00\x02"
            client.send(siyi(1, 23, 0x11, b"\x01\x02"))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x11 and slots_reply[8:10] == b"\x01\x02"
            client.send(siyi(1, 24, 0x11, b"\x03"))
            mode_reply = client.recv(2048)
            assert mode_reply[7] == 0x11 and mode_reply[8] == 3
            client.send(siyi(1, 26, 0x0F, b"\x01\x00"))
            zoom_reply = client.recv(2048)
            assert zoom_reply[7] == 0x0F and zoom_reply[8] == 1
            client.send(siyi(1, 27, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x01\x02"
            client.send(siyi(1, 25, 0x11, b"\x05"))
            mode_reply = client.recv(2048)
            assert mode_reply[7] == 0x11 and mode_reply[8] == 5
            client.send(siyi(1, 16, 0x16))
            range_reply = client.recv(2048)
            assert range_reply[7] == 0x16 and range_reply[8:10] == b"\x0a\x00"
            client.send(siyi(1, 17, 0x0F, b"\x02\x05"))
            zoom_reply = client.recv(2048)
            assert zoom_reply[7] == 0x0F and zoom_reply[8] == 1
            client.send(siyi(1, 28, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x01\x02"
            client.send(siyi(1, 18, 0x18))
            zoom_reply = client.recv(2048)
            assert zoom_reply[7] == 0x18 and zoom_reply[8:10] == b"\x02\x05"
            client.send(siyi(1, 19, 0x0F, b"\x03\x00"))
            assert client.recv(2048)[8] == 1
            client.send(siyi(1, 29, 0x0F, b"\x04\x00"))
            assert client.recv(2048)[8] == 1
            client.send(siyi(1, 30, 0x10))
            slots_reply = client.recv(2048)
            assert slots_reply[7] == 0x10 and slots_reply[8:10] == b"\x00\x02"
            client.send(siyi(1, 20, 0x0F, b"\x01\x00"))
            assert client.recv(2048)[8] == 1
            client.send(siyi(1, 31, 0x0C, b"\x00"))
            capture_reply = client.recv(2048)
            assert capture_reply[7] == 0x0B and capture_reply[8] == 0
            thermal = siyi(1, 14, 0x14, b"\x01")
            client.send(thermal)
            thermal_reply = client.recv(2048)
            assert thermal_reply[7] == 0x14
            assert thermal_reply[8:20] == struct.pack(
                "<HHHHHH", 4200, 1200, 100, 200, 10, 20)
            client.send(siyi(1, 32, 0x37))
            gain_reply = client.recv(2048)
            assert gain_reply[7] == 0x37 and gain_reply[8] == 1
            client.send(siyi(1, 52, 0x1A))
            palette_reply = client.recv(2048)
            assert palette_reply[7] == 0x1A and palette_reply[8] == 0
            client.send(siyi(1, 53, 0x1B, b"\x03"))
            palette_reply = client.recv(2048)
            assert palette_reply[7] == 0x1B and palette_reply[8] == 3
            client.send(siyi(1, 54, 0x1A))
            palette_reply = client.recv(2048)
            assert palette_reply[7] == 0x1A and palette_reply[8] == 3
            client.send(siyi(1, 33, 0x38, b"\x00"))
            client.send(siyi(1, 34, 0x37))
            gain_reply = client.recv(2048)
            assert gain_reply[7] == 0x37 and gain_reply[8] == 0
            client.send(siyi(1, 35, 0x38, b"\x01"))
            client.send(siyi(1, 36, 0x37))
            gain_reply = client.recv(2048)
            assert gain_reply[7] == 0x37 and gain_reply[8] == 1
            unknown = siyi(1, 12, 0x7F, b"\xde\xad")
            client.send(unknown)
            tunneled = read_frames(master, 1)
            assert len(tunneled) == 1 and tunneled[0][12:-2] == unknown
            client.close()
        finally:
            blocker.close()
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=3)
            captured_stderr = stderr
            if process.returncode != 0:
                raise AssertionError(f"camera-app exited {process.returncode}\n{stdout}\n{stderr}")
            os.close(master)
            os.close(slave)
            if external_master >= 0:
                os.close(external_master)
            os.close(external_slave)
        assert ("unknown SIYI command opcode=0x7f control=0x01 sequence=12 "
                "payload_length=2 payload=dead" in captured_stderr)
        assert "timezone=UTC photo_scope=thermal" in captured_stderr
        assert "thermal full-frame temperature not supported" not in captured_stderr
        assert "still capture not supported" not in captured_stderr
        assert captured_stderr.count("HDR not supported") == 1
        assert "thermal range reporting active" in captured_stderr
        assert "E5739 optical zoom movement not supported" not in captured_stderr
        assert "system time setting failed for SIYI opcode=0x30" in captured_stderr
        assert "SIYI UART disabled after I/O failure" in captured_stderr
        assert captured_stderr.startswith("[")
    print("PASS UDP/TCP/UART integration, client routing and camera control")


if __name__ == "__main__":
    main()
