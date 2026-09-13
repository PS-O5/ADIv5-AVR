#!/usr/bin/env python3
"""Tests for tools/gdbserver that need no hardware.

    python3 tools/test_gdbserver.py

The shell reply formats matched here are the real output of cmd_regs,
cmd_break_list, cmd_watch_list and cmd_flash_info in src/shell.c. If those
change, these fail, which is the point: the parsers are coupled to that text
and nothing else would notice.
"""

import os
import pty
import socket
import sys
import termios
import threading
import time
from importlib.machinery import SourceFileLoader
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))

loader = SourceFileLoader("gdbserver", os.path.join(HERE, "gdbserver"))
spec = importlib.util.spec_from_loader("gdbserver", loader)
gs = importlib.util.module_from_spec(spec)
loader.exec_module(gs)


REGS_HALTED = (
    "x\r\n"
    "r0  000000AA  r1  20000000  r2  00000000  r3  00000000\r\n"
    "r4  00000000  r5  00000000  r6  00000000  r7  00000000\r\n"
    "r8  00000000  r9  00000000  r10 00000000  r11 00000000\r\n"
    "r12 00000000  sp  20001000  lr  FFFFFFFF  pc  0800000C\r\n"
    "psr 01000000  \r\n> ")

FLASH_INFO = (
    "f\r\n512KB, 8 sectors\r\n"
    "  0  0x08000000  16KB\r\n"
    "  1  0x08004000  16KB\r\n"
    "  4  0x08010000  64KB\r\n"
    "  5  0x08020000  128KB\r\n> ")


class Shell:
    """A scripted shell. Replies carry the command echo, like the real one:
    leaving it out once hid a parser that only matched at position zero."""

    def __init__(self, mapping):
        self.mapping = list(mapping)
        self.log = []

    def send(self, cmd, timeout=5.0, retries=1):
        self.log.append(cmd)
        for prefix, reply in self.mapping:
            if cmd.startswith(prefix):
                return reply
        raise AssertionError("unscripted command: %r" % cmd)


class Rsp:
    def check_interrupt(self):
        return False


class Flash:
    """Programming clears bits and never sets them, so an unerased cell keeps
    its zeros and reports success. Verification exists for exactly that."""

    def __init__(self, preloaded=None):
        self.log = []
        self.reset = False
        self.unlocked = False
        self.cells = dict(preloaded or {})

    def send(self, cmd, timeout=5.0, retries=1):
        self.log.append(cmd)

        if cmd == "f":
            return FLASH_INFO
        if cmd == "t":
            self.reset = True
            self.unlocked = False      # a reset relocks the controller
            return "t\r\nok\r\n> "
        if cmd == "u":
            if not self.reset:
                return "u\r\nfailed, ack=0x04\r\n> "
            self.unlocked = True
            return "u\r\nok\r\n> "
        if cmd.startswith("e "):
            if not self.unlocked:
                return "e\r\nfailed, ack=0xFC\r\n> "
            for a in list(self.cells):
                self.cells[a] = 0xFF
            return "e\r\nok\r\n> "
        if cmd.startswith("p "):
            if not self.unlocked:
                return "p\r\nfailed, ack=0xFC\r\n> "
            _, addr_s, val_s = cmd.split()
            addr, val = int(addr_s, 16), int(val_s, 16)
            for i in range(4):
                byte = (val >> (8 * i)) & 0xFF
                self.cells[addr + i] = self.cells.get(addr + i, 0xFF) & byte
            return "p\r\nok\r\n> "
        if cmd.startswith("d "):
            _, addr_s, count_s = cmd.split()
            addr, count = int(addr_s, 16), int(count_s)
            words = []
            for w in range(count):
                base = addr + 4 * w
                words.append("%08X" % sum(
                    self.cells.get(base + i, 0xFF) << (8 * i) for i in range(4)))
            return "d\r\n0x%08X: %s\r\n> " % (addr, " ".join(words))

        raise AssertionError("unscripted command: %r" % cmd)


# ---------------------------------------------------------------------------

def test_parse_registers():
    values = gs.parse_registers(REGS_HALTED)
    assert values == [0xAA, 0x20000000] + [0] * 11 + [
        0x20001000, 0xFFFFFFFF, 0x0800000C, 0x01000000], values
    assert gs.parse_registers("x\r\ncore is running, halt first\r\n> ") is None


def test_parse_listings():
    watch = ("a\r\n4 watchpoint slots\r\n"
             "  0  write  0x20000000  matched\r\n"
             "  1  read   0x20000200\r\n"
             "  2  off  \r\n> ")
    assert gs.parse_watchpoints(watch) == [
        ("write", "0x20000000", True), ("read", "0x20000200", False)]
    assert gs.find_watchpoint_slot(watch, "0x20000200") == 1

    brk = "b\r\nfpb rev 0, 6 slots\r\n  0  enabled  0x0800000A\r\n  1  free     \r\n> "
    assert gs.parse_breakpoints(brk) == ["0x0800000A"]
    assert gs.find_breakpoint_slot(brk, "0x0800000A") == 0


def test_hex_helpers():
    assert gs.word_to_le_hex(0x11223344) == "44332211"
    assert gs.le_hex_to_word("44332211") == 0x11223344
    assert gs.checksum(b"OK") == sum(b"OK") & 0xFF


def test_classify_stop():
    regs = gs.parse_registers(REGS_HALTED)

    matched = ("a\r\n  0  write  0x20000000  matched\r\n> ",
               "b\r\nfpb rev 0, 6 slots\r\n  0  enabled  0x0800000C\r\n> ")
    bridge = gs.Bridge(Shell([("a", matched[0]), ("b", matched[1])]))
    assert bridge.classify_stop(regs) == ("watch", 0x20000000)

    bridge = gs.Bridge(Shell([
        ("a", "a\r\n  0  off  \r\n> "),
        ("b", "b\r\nfpb rev 0, 6 slots\r\n  0  enabled  0x0800000C\r\n> ")]))
    assert bridge.classify_stop(regs) == ("hwbreak", None)

    bridge = gs.Bridge(Shell([
        ("a", "a\r\n  0  off  \r\n> "),
        ("b", "b\r\nfpb rev 0, 6 slots\r\n  0  free     \r\n> ")]))
    assert bridge.classify_stop(regs) == (None, None)


def test_stop_reply():
    regs = gs.parse_registers(REGS_HALTED)
    assert gs.stop_reply(regs, ("watch", 0x20000000)) == "T050f:0c000008;watch:20000000;"
    assert gs.stop_reply(regs, ("hwbreak", None)) == "T050f:0c000008;hwbreak:;"
    assert gs.stop_reply(regs, None) == "T050f:0c000008;"
    assert gs.stop_reply(None) == "S05"


def test_read_memory():
    bridge = gs.Bridge(Shell([
        ("d 20000000", "d\r\n0x20000000: 11223344 55667788\r\n> ")]))
    # 0x11223344 little endian puts 0x44 at the lowest address, so bytes
    # one through three read back as 33 22 11
    assert bridge.read_memory(0x20000001, 3) == "332211"
    assert bridge.read_memory(0x20000000, 4) == "44332211"

    bridge = gs.Bridge(Shell([("d ", "d\r\n0x20000000: --------\r\n> ")]))
    assert bridge.read_memory(0x20000000, 4) is None


def test_binary_unescape():
    for special in (0x24, 0x23, 0x7D, 0x2A):
        assert gs.binary_unescape(bytes([0x7D, special ^ 0x20])) == bytes([special])
    assert gs.binary_unescape(bytes(range(10))) == bytes(range(10))


def test_memory_map_covers_what_we_touch():
    xml = gs.build_memory_map_xml([(0, 0x08000000, 0x4000),
                                   (4, 0x08010000, 0x10000)]).decode()
    assert 'type="flash"' in xml and 'type="rom"' not in xml.split("ram")[0]
    assert '<property name="blocksize">0x4000</property>' in xml

    import re
    regions = [(kind, int(start, 16), int(length, 16)) for kind, start, length
               in re.findall(r'type="(\w+)" start="(0x[0-9a-f]+)" length="(0x[0-9a-f]+)"',
                             xml)]

    def covered(addr):
        return any(s <= addr < s + n for _, s, n in regions)

    # gdb refuses any address the map omits, without sending a packet, so a
    # missing region is indistinguishable from a target that cannot be read.
    for addr in (0x08000000, 0x20000000, 0x2001FFFF, 0x40023830, 0x40020814,
                 0x1FFF7A22, 0xE000EDF0, 0xE0001000, 0xE0002000, 0xE00FF000):
        assert covered(addr), "0x%08X missing from the memory map" % addr


def test_qxfer_chunking():
    doc = b"0123456789"
    assert gs.qxfer_chunk(doc, 0, 4) == "m0123"
    assert gs.qxfer_chunk(doc, 4, 100) == "l456789"
    assert gs.qxfer_chunk(doc, 10, 5) == "l"


def test_dispatch():
    bridge = gs.Bridge(Shell([
        ("x pc", "ok\r\n> "), ("x r", "ok\r\n> "), ("x sp", "ok\r\n> "),
        ("x lr", "ok\r\n> "), ("x psr", "ok\r\n> "), ("x", REGS_HALTED),
        ("d 20000000", "d\r\n0x20000000: 11223344\r\n> "),
        ("w 20000000", "w\r\nok\r\n> "),
        ("b 800000a", "b\r\nbreakpoint 0 at 0x0800000A\r\n> "),
        ("b", "b\r\nfpb rev 0, 6 slots\r\n  0  enabled  0x0800000A\r\n> "),
        ("k 0", "ok\r\n> "),
        ("a 20000000", "a\r\nwatchpoint 0 on write  at 0x20000000\r\n> "),
        ("a", "a\r\n4 watchpoint slots\r\n  0  write  0x20000000\r\n> "),
        ("j 0", "ok\r\n> "),
    ]))
    rsp = Rsp()

    assert "PacketSize" in gs.handle(bridge, rsp, b"qSupported")
    assert "qXfer:memory-map:read+" in gs.handle(bridge, rsp, b"qSupported")
    assert gs.handle(bridge, rsp, b"?").startswith("T05")
    # an empty reply to T means no such thread, and gdb reports it as exited
    assert gs.handle(bridge, rsp, b"T1") == "OK"
    assert gs.handle(bridge, rsp, b"qC") == "QC1"
    assert len(gs.handle(bridge, rsp, b"g")) == 17 * 8

    packet = "".join(gs.word_to_le_hex(v) for v in
                     [1, 2] + [0] * 11 + [0x20001000, 0xFFFFFFFF, 0x0800000C, 0x01000000])
    assert gs.handle(bridge, rsp, b"G" + packet.encode()) == "OK"

    assert gs.handle(bridge, rsp, b"m20000000,4") == "44332211"
    assert gs.handle(bridge, rsp, b"M20000000,4:44332211") == "OK"

    assert gs.handle(bridge, rsp, b"Z1,800000a,2") == "OK"
    assert gs.handle(bridge, rsp, b"z1,800000a,2") == "OK"
    assert gs.handle(bridge, rsp, b"Z2,20000000,4") == "OK"
    assert bridge.avr.log[-1] == "a 20000000 w 4", bridge.avr.log[-1]
    assert gs.handle(bridge, rsp, b"z2,20000000,4") == "OK"

    assert gs.handle(bridge, rsp, b"vCont?").startswith("vCont;")
    assert gs.handle(bridge, rsp, b"nonsense") == ""


def test_watchpoint_length_is_hex():
    bridge = gs.Bridge(Shell([("a ", "a\r\nwatchpoint 0 on write  at 0x20000000\r\n> ")]))
    gs.handle(bridge, Rsp(), b"Z3,20000000,10")
    assert bridge.avr.log[-1] == "a 20000000 r 16", bridge.avr.log[-1]


def test_flash_probe_ignores_the_echo():
    bridge = gs.Bridge(Flash())
    assert bridge.probe_flash() == 512 * 1024
    assert bridge.flash_sectors[0] == (0, 0x08000000, 16 * 1024)
    assert bridge.flash_sectors[2] == (4, 0x08010000, 64 * 1024)


def test_flash_write_order_and_verify():
    avr = Flash()
    bridge = gs.Bridge(avr)
    bridge.probe_flash()
    gs.flash_size = 512 * 1024

    data = bytes.fromhex("00000220090000080f4ad2f8")
    assert gs.handle(bridge, Rsp(), b"vFlashErase:08000000,00004000") == "OK"
    assert gs.handle(bridge, Rsp(), b"vFlashWrite:8000000:" + data) == "OK"
    assert gs.handle(bridge, Rsp(), b"vFlashDone") == "OK"

    order = [c for c in avr.log if c in ("t", "u") or c.startswith(("e ", "p "))]
    assert order[0] == "t" and order[1] == "u", order
    # the reset relocks the controller, so doing it twice would undo the unlock
    assert avr.log.count("t") == 1
    assert avr.log.count("e 0") == 1

    readback = bytes(avr.cells.get(0x08000000 + i, 0xFF) for i in range(len(data)))
    assert readback == data


def test_flash_write_is_word_padded_with_ones():
    avr = Flash()
    bridge = gs.Bridge(avr)
    bridge.probe_flash()
    bridge.erased_sectors.add(0)
    bridge.flash_unlocked = True
    avr.unlocked = True

    assert bridge.write_flash(0x08000002, bytes.fromhex("aabbcc"))
    first = [avr.cells.get(0x08000000 + i, 0xFF) for i in range(4)]
    second = [avr.cells.get(0x08000004 + i, 0xFF) for i in range(4)]
    # 0xFF clears no bits, so the bytes either side are left as they were
    assert first == [0xFF, 0xFF, 0xAA, 0xBB], first
    assert second == [0xCC, 0xFF, 0xFF, 0xFF], second


def test_verify_catches_a_cell_that_did_not_take():
    avr = Flash(preloaded={0x08000002: 0x00})
    bridge = gs.Bridge(avr)
    bridge.probe_flash()
    bridge.erased_sectors.add(0)     # claim it was erased when it was not
    bridge.flash_unlocked = True
    avr.unlocked = True

    assert bridge.write_flash(0x08000000, bytes.fromhex("00000220")) is False


def test_locked_controller_fails_rather_than_pretending():
    class Locked(Flash):
        def send(self, cmd, timeout=5.0, retries=1):
            if cmd == "u":
                self.log.append(cmd)
                return "u\r\nfailed, ack=0xFC\r\n> "
            return super().send(cmd, timeout, retries)

    bridge = gs.Bridge(Locked())
    bridge.probe_flash()
    assert gs.handle(bridge, Rsp(), b"vFlashErase:08000000,00004000") == "E01"


def test_rsp_framing():
    left, right = socket.socketpair()
    left.settimeout(5)
    right.settimeout(5)
    conn = gs.RspConn(left)

    payload = b"qSupported"
    right.sendall(b"$" + payload + b"#" + ("%02x" % gs.checksum(payload)).encode())
    assert conn.read_packet(timeout=2.0) == payload
    assert right.recv(1) == b"+"

    right.sendall(b"$g#00")                      # wrong checksum
    assert conn.read_packet(timeout=1.5) is None
    assert right.recv(1) == b"-"

    right.sendall(b"$g#" + ("%02x" % gs.checksum(b"g")).encode())
    assert conn.read_packet(timeout=2.0) == b"g"
    assert right.recv(1) == b"+"

    conn.send_packet("OK")
    assert right.recv(64) == b"$OK#" + ("%02x" % gs.checksum(b"OK")).encode()
    right.sendall(b"+")
    time.sleep(0.05)

    left.close()
    right.close()


def test_a_late_reply_cannot_answer_the_next_command():
    """A reply that arrives after its own read gave up stays in the buffer, and
    reading it as the next command's answer shifts every reply after it."""
    master, slave = pty.openpty()

    def target():
        buf = b""
        while True:
            try:
                chunk = os.read(master, 256)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\r" in buf:
                line, _, buf = buf.partition(b"\r")
                cmd = line.decode().strip()
                if cmd == "slow":
                    time.sleep(1.0)
                    os.write(master, b"slow\r\nok\r\n> ")
                else:
                    os.write(master, b"d\r\n0x20000000: 11223344\r\n> ")

    threading.Thread(target=target, daemon=True).start()
    shell = gs.AvrShell(os.ttyname(slave))

    try:
        shell.send("slow", timeout=0.3, retries=0)
        raise AssertionError("expected the slow command to time out")
    except RuntimeError:
        pass

    time.sleep(1.2)      # the late reply lands while gdb is thinking
    assert "0x20000000" in shell.send("d 20000000 1", retries=0)

    shell.close()
    os.close(master)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0

    for test in tests:
        name = test.__name__[len("test_"):].replace("_", " ")
        try:
            test()
        except Exception as exc:            # noqa: BLE001 - report and continue
            failed += 1
            print("FAIL  %s: %s" % (name, exc))
        else:
            print("ok    %s" % name)

    print()
    if failed:
        print("%d of %d failed" % (failed, len(tests)))
        return 1

    print("%d tests passed" % len(tests))
    return 0


if __name__ == "__main__":
    sys.exit(main())
