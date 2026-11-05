"""M1 设备识别器命令行入口。

    python cli.py list [--json]            列出全部 HID 接口 + 指纹状态
    python cli.py info <VID:PID> [--desc]  单设备各接口详情（--desc 读描述符）
    python cli.py watch [--interval 1.0]   热插拔监视（Ctrl+C 退出）

组合 core/ 三个模块，自身不含业务逻辑。
"""
from __future__ import annotations

import argparse
import json
import sys

import hid

# GBK 控制台兜底：任何字符都打得出来（repo 内含 ⚠ 等符号）
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

import core.device_manager as dm
from core.battery import scan_all
from core.descriptor import parse_report_descriptor, summary
from core.fingerprint import (
    DEFAULT_DB_PATH,
    DeviceStatus,
    fingerprint,
    load_db,
)

STATUS_MARK = {
    DeviceStatus.KNOWN: "OK ",
    DeviceStatus.SDK: "SDK",
    DeviceStatus.UNKNOWN: "???",
}


def parse_vidpid(text: str) -> tuple[int, int]:
    """"291D:385D" / "0x291D:0x385d" -> (vid, pid)；非法抛 ValueError。"""
    parts = text.strip().lower().split(":")
    if len(parts) != 2:
        raise ValueError(f"格式应为 VID:PID，例如 046D:C54D，收到: {text!r}")
    vid, pid = parts
    return int(vid, 16), int(pid, 16)


def usage_short(usage_page: int, usage: int) -> str:
    """usage 显示：厂商页标 V 前缀。"""
    mark = "V" if usage_page >= 0xFF00 else " "
    return f"{usage_page:04X}:{usage:04X}{mark}"


def cmd_list(args) -> int:
    db = load_db(DEFAULT_DB_PATH)
    devices = dm.list_devices()
    if args.json:
        payload = {
            "count": len(devices),
            "devices": [
                {
                    "vid": d.vid_hex, "pid": d.pid_hex,
                    "interface": d.interface_number,
                    "product": d.product,
                    "manufacturer": d.manufacturer,
                    "serial": d.serial_number,
                    "usage_page": f"0x{d.usage_page:04X}",
                    "usage": f"0x{d.usage:04X}",
                    "path": d.path,
                    **_fingerprint_dict(d, db),
                }
                for d in devices
            ],
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0

    print(f"共 {len(devices)} 个 HID 接口")
    print(f"{'状态':<4} {'VID:PID':<10} {'IF':>2}  {'产品':<28} {'UP:USAGE':<11} 说明")
    print("-" * 96)
    for d in devices:
        result = fingerprint(d, db)
        mark = STATUS_MARK[result.status]
        print(
            f"{mark:<4} {d.vid_hex}:{d.pid_hex:<5} {d.interface_number:>2}  "
            f"{(d.product or '(未命名)')[:28]:<28} "
            f"{usage_short(d.usage_page, d.usage):<11} {result.detail}"
        )
    print("-" * 96)
    print("OK=已知(可直通)  SDK=有官方SDK  ???=未知(可学习)；UP:USAGE 末尾 V=厂商自定义页")
    return 0


def _fingerprint_dict(device, db) -> dict:
    result = fingerprint(device, db)
    return {
        "status": result.status.value,
        "brand": result.brand,
        "brand_zh": result.brand_zh,
        "sdk": list(result.sdk),
        "detail": result.detail,
    }


def cmd_info(args) -> int:
    db = load_db(DEFAULT_DB_PATH)
    try:
        vid, pid = parse_vidpid(args.vidpid)
    except ValueError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1

    devices = dm.find_devices(vendor_id=vid, product_id=pid)
    if not devices:
        print(f"未找到设备 {args.vidpid}", file=sys.stderr)
        return 1

    result = fingerprint(devices[0], db)
    print(f"设备 {vid:04X}:{pid:04X}  品牌: {result.brand_zh or result.brand or '?'}  "
          f"状态: {STATUS_MARK[result.status]}  {result.detail}")
    if result.entry:
        print(f"  条目备注: {result.entry.get('note', '(无)')}")

    for d in devices:
        print(f"\n── 接口 {d.interface_number}  usage_page=0x{d.usage_page:04X} "
              f"usage=0x{d.usage:04X}  序列号={d.serial_number or '-'}")
        if not args.desc:
            continue
        raw = _read_descriptor(d.path)
        if raw is None:
            print("   (描述符读取失败：设备被系统占用或接口不可读)")
            continue
        info = parse_report_descriptor(raw)
        for line in summary(info):
            print(f"   {line}")
    return 0


def _read_descriptor(path: str) -> bytes | None:
    """打开设备只读描述符；失败返回 None（M1 绝不写、不做别的）。"""
    try:
        dev = hid.device()
        dev.open_path(path.encode("utf-8") if isinstance(path, str) else path)
        try:
            raw = dev.get_report_descriptor()
        finally:
            dev.close()
        return bytes(raw)
    except Exception:
        return None


def cmd_watch(args) -> int:
    db = load_db(DEFAULT_DB_PATH)
    print(f"热插拔监视中（轮询 {args.interval}s）… Ctrl+C 退出")

    def on_event(event: dm.DeviceEvent) -> None:
        d = event.device
        result = fingerprint(d, db)
        mark = STATUS_MARK[result.status]
        arrow = "＋接入" if event.kind == "added" else "－移除"
        print(f"{arrow} [{mark}] {d.vid_hex}:{d.pid_hex} "
              f"if={d.interface_number} {(d.product or '(未命名)')}  {result.detail}")

    try:
        dm.watch_devices(on_event, interval=args.interval)
    except KeyboardInterrupt:
        print("\n已退出")
    return 0


def cmd_battery(args) -> int:
    """全部设备电量扫描（M2.5a）。"""
    devices = dm.list_devices()
    grouped: dict[str, list] = {}
    for d in devices:
        grouped.setdefault(f"{d.vid_hex}:{d.pid_hex}", []).append(d)

    def read_descriptor(path: str) -> bytes | None:
        try:
            dev = hid.device()
            dev.open_path(path.encode("utf-8") if isinstance(path, str) else path)
            try:
                return bytes(dev.get_report_descriptor())
            finally:
                dev.close()
        except Exception:
            return None

    results = scan_all(grouped, read_descriptor)
    print(f"{'VID:PID':<10} {'设备':<26} {'电量':<9} 来源/说明")
    print("-" * 84)
    for r in results:
        print(f"{r.vidpid:<10} {r.product[:26]:<26} {r.display:<9} {r.detail}")
    print("-" * 84)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cli.py", description="M1 设备识别器：枚举/识别/监视 HID 外设（只读）"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="列出全部 HID 接口 + 指纹状态")
    p_list.add_argument("--json", action="store_true", help="输出 JSON")

    p_info = sub.add_parser("info", help="单设备详情")
    p_info.add_argument("vidpid", help='设备 "VID:PID"，如 046D:C54D')
    p_info.add_argument("--desc", action="store_true", help="读取并解析报告描述符")

    p_watch = sub.add_parser("watch", help="热插拔监视")
    p_watch.add_argument("--interval", type=float, default=1.0, help="轮询秒数")

    sub.add_parser("battery", help="全部设备电量扫描")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return {"list": cmd_list, "info": cmd_info, "watch": cmd_watch,
                "battery": cmd_battery}[args.command](args)
    except dm.HIDError as exc:
        print(f"HID 错误: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # 顶层兜底：转人话，不留栈给用户
        print(f"错误: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
