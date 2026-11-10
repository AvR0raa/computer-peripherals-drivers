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
from core.hidpp import (
    HidppLink,
    ensure_online,
    get_poll_rates,
    read_battery,
    read_poll_rate,
    set_poll_rate,
)
from core.dpi import get_dpi_list, read_dpi
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


def cmd_mouse(args) -> int:
    """罗技鼠标完整报告：唤醒 → 协议/DPI/电量（20 秒唤醒窗口）。"""
    import time
    ifaces = [d for d in dm.list_devices()
              if d.vid_hex == "046D" and d.pid_hex == "C54D"]
    short = next((d for d in ifaces if d.usage == 0x0001), None)
    long_if = next((d for d in ifaces if d.usage == 0x0002), None)
    if short is None and long_if is None:
        print("未找到 C54D 接收器", file=sys.stderr)
        return 1
    link = HidppLink(short.path if short else None, long_if.path if long_if else None)
    dev = args.slot if args.slot else 0xFF
    print(f"连接接收器（槽位 {dev if dev != 0xFF else '0xFF/直连'}）…")
    print(">>> 请反复移动并点击鼠标（约 12 秒唤醒窗口）<<<")
    with link:
        version = ensure_online(link, dev, attempts=10, interval=1.2)
        if version is None:
            print("设备不在线。确认: ①鼠标开着 ②已配对到该接收器 ③正在动鼠标")
            return 1
        print(f"✅ 在线，HID++ {version}")
        dpi, dpi_y, lod, default = read_dpi(link, dev)
        if dpi is not None:
            print(f"DPI: {dpi}" + (f"  Y轴: {dpi_y}" if dpi_y else "")
                  + (f"  LOD: {lod}" if lod is not None else "")
                  + (f"  默认: {default}" if default else ""))
        else:
            print("DPI: 读取失败（0x2201/0x2202 均无响应）")
        lst = get_dpi_list(link, dev)
        if lst:
            print(f"档位表: {len(lst)} 档，{lst[0]} - {lst[-1]}")
        rate = read_poll_rate(link, dev)
        rates = get_poll_rates(link, dev)
        if rate:
            print(f"报告率: {rate}Hz" + (f"（支持 {rates}）" if rates else ""))
        if args.rate is not None:
            print("")
            print(f"设置报告率 → {args.rate}Hz …")
            ok, msg = set_poll_rate(link, dev, args.rate)
            print("  " + ("✅ " if ok else "❌ ") + msg)
            if not ok:
                return 1
        b = read_battery(link, dev)
        if b:
            print(f"电量: {b.percent if b.percent is not None else b.display} "
                  f"{'充电中' if b.charging else ''} ({b.source})")
        else:
            print("电量: 无电量 feature")
        if args.set is not None:
            from core.dpi import set_dpi
            print(f"\n设置 DPI → {args.set}（带读回验证+回滚）…")
            outcome = set_dpi(link, dev, args.set)
            for line in outcome.log:
                print("  " + line)
            return 0 if outcome.ok else 1
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

    p_mouse = sub.add_parser("mouse", help="罗技鼠标报告（唤醒/DPI/电量）+ 可选设置 DPI")
    p_mouse.add_argument("--slot", type=int, default=1, help="接收器槽位 1-7（0=直连 0xFF）")
    p_mouse.add_argument("--set", type=int, default=None, help="设置 DPI 到指定值（可选）")
    p_mouse.add_argument("--rate", type=int, default=None, help="设置报告率 Hz（125/250/500/1000/2000/4000/8000）")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return {"list": cmd_list, "info": cmd_info, "watch": cmd_watch,
                "battery": cmd_battery, "mouse": cmd_mouse}[args.command](args)
    except dm.HIDError as exc:
        print(f"HID 错误: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # 顶层兜底：转人话，不留栈给用户
        print(f"错误: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
