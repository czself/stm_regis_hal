#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 64 课配套上位机：串口 OTA 升级脚本。

协议（与 bootloader.c 完全一致）：
  请求帧：AA 55 | CMD(1) | SEQ(2,LE) | LEN(2,LE) | PAYLOAD | CRC16(2,LE)
  应答帧：AA 55 | CMD|0x80 | SEQ(2,LE) | LEN(2,LE) | STATUS(1) | CRC16(2,LE)
  CRC16：多项式 0x1021，初值 0xFFFF，覆盖 CMD/SEQ/LEN/PAYLOAD。

用法：
  pip install pyserial
  python ota_upload.py --port /dev/ttyUSB0 --file app.bin
  python ota_upload.py --port COM7 --file app.bin --dtr-reset
"""

import argparse
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("缺少 pyserial，请先执行：pip install pyserial")
    sys.exit(1)

SYNC = b"\xAA\x55"

CMD_HELLO = 0x01
CMD_DATA = 0x02
CMD_END = 0x03
CMD_ABORT = 0x04

STATUS_TEXT = {
    0x00: "OK",
    0x01: "CRC16 错误",
    0x02: "长度错误",
    0x03: "序号错误",
    0x04: "状态错误",
    0x05: "Flash 操作失败",
    0x06: "镜像大小错误",
    0x07: "整片 CRC32 校验失败",
}


def crc16(data: bytes) -> int:
    """CRC16-CCITT-FALSE，和 MCU 端逐位实现保持一致。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, seq: int, payload: bytes) -> bytes:
    body = struct.pack("<BHH", cmd, seq, len(payload)) + payload
    return SYNC + body + struct.pack("<H", crc16(body))


class OtaClient:
    def __init__(self, port: str, baud: int, timeout: float = 1.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _read_exact(self, count: int, deadline: float) -> bytes:
        data = b""
        while len(data) < count:
            if time.time() > deadline:
                return b""
            chunk = self.ser.read(count - len(data))
            if chunk:
                data += chunk
        return data

    def read_frame(self, timeout: float):
        """返回 (cmd, seq, payload) 或 None。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            first = self.ser.read(1)
            if not first:
                continue
            if first != b"\xAA":
                continue
            second = self._read_exact(1, deadline)
            if second != b"\x55":
                continue

            header = self._read_exact(5, deadline)
            if len(header) != 5:
                return None
            cmd, seq, length = struct.unpack("<BHH", header)
            payload = self._read_exact(length, deadline)
            crc_bytes = self._read_exact(2, deadline)
            if len(payload) != length or len(crc_bytes) != 2:
                return None

            recv_crc = struct.unpack("<H", crc_bytes)[0]
            calc_crc = crc16(header + payload)
            if recv_crc != calc_crc:
                continue
            return cmd, seq, payload
        return None

    def request(self, cmd: int, seq: int, payload: bytes, timeout: float,
                retries: int = 3):
        """发送一帧并等待同序号 ACK；返回 status，失败返回 None。"""
        expected_cmd = cmd | 0x80
        for attempt in range(1, retries + 1):
            self.ser.reset_input_buffer()
            self.ser.write(build_frame(cmd, seq, payload))
            self.ser.flush()

            deadline = time.time() + timeout
            while time.time() < deadline:
                frame = self.read_frame(deadline - time.time())
                if frame is None:
                    break
                rcmd, rseq, rpayload = frame
                if rcmd != expected_cmd or rseq != seq or len(rpayload) != 1:
                    # 可能是上一次重传的迟到应答，继续等匹配帧
                    continue
                return rpayload[0]
            if attempt < retries:
                print(f"  重试 {attempt}/{retries} ...")
        return None


def load_firmware(path: str) -> bytes:
    with open(path, "rb") as fp:
        data = fp.read()
    if not data:
        raise ValueError("固件文件为空")
    return data


def upgrade(args) -> int:
    image = load_firmware(args.file)
    image_crc = zlib.crc32(image) & 0xFFFFFFFF

    print(f"固件：{args.file}")
    print(f"大小：{len(image)} 字节")
    print(f"CRC32：0x{image_crc:08X}")
    print(f"串口：{args.port} @ {args.baud}")

    client = OtaClient(args.port, args.baud, args.timeout)

    if args.dtr_reset:
        print("通过 DTR 复位开发板 ...")
        client.ser.dtr = False
        time.sleep(0.1)
        client.ser.dtr = True
        time.sleep(0.3)

    print("\n[1/3] 等待 Bootloader 并发送 HELLO")
    print("如果板子已经在运行 app，请手动按一下复位键。")

    hello_payload = struct.pack("<II", len(image), image_crc)
    status = None
    for _ in range(args.handshake_tries):
        status = client.request(CMD_HELLO, 0, hello_payload, 3.0, retries=1)
        if status == 0x00:
            break
        if status is not None:
            print(f"  Bootloader 拒绝 HELLO：{STATUS_TEXT.get(status, status)}")
            client.close()
            return 1
        time.sleep(0.3)

    if status != 0x00:
        print("未握手成功：确认串口、波特率、接线，并在 5 秒窗口内复位。")
        client.close()
        return 1
    print("  HELLO OK，app 区已擦除")

    print("\n[2/3] 发送固件数据")
    offset = 0
    seq = 0
    total = len(image)
    started = time.time()

    while offset < total:
        chunk = image[offset:offset + args.chunk]
        status = client.request(CMD_DATA, seq, chunk, args.timeout, args.retries)
        if status != 0x00:
            text = STATUS_TEXT.get(status, "无应答") if status is not None else "无应答"
            print(f"\n  第 {seq} 包失败：{text}")
            client.request(CMD_ABORT, 0, b"", 1.0, retries=1)
            client.close()
            return 1

        offset += len(chunk)
        seq += 1
        percent = offset * 100 // total
        print(f"\r  进度 {percent:3d}%  {offset}/{total} 字节", end="", flush=True)

    elapsed = max(time.time() - started, 1e-6)
    print(f"\n  传输完成，用时 {elapsed:.1f}s，平均 {total / elapsed / 1024:.1f} KB/s")

    print("\n[3/3] 请求校验并跳转")
    status = client.request(CMD_END, seq, b"", 5.0, args.retries)
    if status != 0x00:
        text = STATUS_TEXT.get(status, "无应答") if status is not None else "无应答"
        print(f"  END 失败：{text}")
        client.close()
        return 1

    print("  校验通过，Bootloader 已跳转到新固件")
    time.sleep(0.5)
    tail = client.ser.read(512)
    if tail:
        print("  新固件串口输出：")
        print("  " + tail.decode("utf-8", errors="replace").replace("\r", ""))

    client.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description="STM32 UART OTA 上位机")
    parser.add_argument("--port", required=True, help="串口设备，如 /dev/ttyUSB0 或 COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--file", required=True, help="要升级的 bin 文件")
    parser.add_argument("--chunk", type=int, default=128, help="每包数据字节数，最大 128")
    parser.add_argument("--timeout", type=float, default=1.5, help="单包 ACK 超时（秒）")
    parser.add_argument("--retries", type=int, default=3, help="每包重试次数")
    parser.add_argument("--handshake-tries", type=int, default=20, help="HELLO 尝试次数")
    parser.add_argument("--dtr-reset", action="store_true", help="用 DTR 复位开发板（DTR 需接 NRST）")
    args = parser.parse_args()

    if args.chunk <= 0 or args.chunk > 128 or args.chunk % 2 != 0:
        print("--chunk 必须是 2~128 之间的偶数")
        return 2

    try:
        return upgrade(args)
    except KeyboardInterrupt:
        print("\n用户中断")
        return 130
    except Exception as exc:
        print(f"升级失败：{exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
