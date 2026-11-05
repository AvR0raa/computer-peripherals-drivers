"""HID++ 脚本化应答构造器：与 core.hidpp 的 sw_id 轮换天然对齐。

原理：被测代码按顺序消耗 sw_id(1-7 循环)；本类按"应答顺序"预测同一序列，
双方一一对应。每个测试 new 一个 HidppScript（计数器归零）。
"""
import core.hidpp as h


class HidppScript:
    def __init__(self):
        self.n = 0
        h._sw_state["n"] = 0

    def _sw(self) -> int:
        self.n += 1
        return ((self.n - 1) % 7) + 1     # SUT 第 k 次消耗得到 ((k-1)%7)+1

    def gf(self, dev: int, feat_index: int) -> list:
        """Root.GetFeature 应答（funct 0）。feat_index=0 表示不支持。"""
        return [0x10, dev, 0x00, self._sw(), feat_index, 0, 0]

    def call(self, dev: int, feat_index: int, funct: int, payload: bytes,
             long: bool = False) -> list:
        """feature 调用应答：payload = req-id 回显之后的数据段。"""
        rid = 0x11 if long else 0x10
        wire = bytes([rid, dev, feat_index, ((funct & 0x0F) << 4) | self._sw()]) + payload
        return list(wire.ljust(20 if long else 7, b"\x00"))

    def ping(self, dev: int, major: int, minor: int, long: bool = False,
             mark: int = 66) -> list:
        """ping 应答（PRO X2 实测不回显 mark，构造器按需带）。"""
        rid = 0x11 if long else 0x10
        wire = bytes([rid, dev, 0x00, 0x10 | self._sw(), major, minor, mark])
        return list(wire.ljust(20 if long else 7, b"\x00"))

    def err(self, dev: int, error_code: int, long: bool = False) -> list:
        """0x8F 错误应答。"""
        rid = 0x11 if long else 0x10
        wire = bytes([rid, dev, 0x8F, 0x00, 0x10, error_code, 0x00])
        return list(wire.ljust(20 if long else 7, b"\x00"))
