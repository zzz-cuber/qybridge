#!/usr/bin/env python3
"""qybridge 串口日志抓取工具（抗掉线版）

用法:
    python serial_capture.py COM6            # 抓 15 秒, 打印到屏幕
    python serial_capture.py COM6 60 log.txt # 抓 60 秒, 同时增量写文件

特点:
  * 先给一个复位脉冲, 保证从开机第一行开始抓
  * 串口掉线(板子重启/USB 抖动)不会退出, 会自动重连并继续抓
  * 数据是"边收边写"的, 程序被中断也不会丢已抓到的部分
比赛现场排查 DNF 时建议一直开着, 出问题直接把 log.txt 发出来。
"""
import sys
import time

import serial


def open_port(port: str, reset: bool) -> serial.Serial:
    while True:
        try:
            ser = serial.Serial(port, 115200, timeout=0.2)
            if reset:
                try:
                    ser.dtr = False
                    ser.rts = True
                    time.sleep(0.1)
                    ser.rts = False
                except Exception:
                    pass
            return ser
        except Exception as exc:  # 端口暂时不在(掉线/重枚举)
            print(f"\n[等待 {port}: {exc}]", file=sys.stderr, flush=True)
            time.sleep(0.5)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
    out_path = sys.argv[3] if len(sys.argv) > 3 else None

    if out_path:
        open(out_path, "wb").close()   # 清空

    ser = open_port(port, reset=True)
    total = 0
    start = time.time()
    try:
        while time.time() - start < seconds:
            try:
                data = ser.read(4096)
            except Exception as exc:
                print(f"\n[串口断开: {exc}; 重连中...]", file=sys.stderr, flush=True)
                try:
                    ser.close()
                except Exception:
                    pass
                ser = open_port(port, reset=False)
                continue
            if data:
                total += len(data)
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
                if out_path:
                    with open(out_path, "ab") as f:
                        f.write(data)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass
        print(f"\n[已抓 {total} 字节" + (f", 保存到 {out_path}]" if out_path else "]"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
