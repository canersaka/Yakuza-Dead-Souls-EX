#!/usr/bin/env python3
"""Drive RPCS3's GDB stub to the exact orphanage renderer checkpoint.

This does not read or diff whole guest memory.  It stops the PPU at 0x113584,
records registers/thread/frame-adjacent metadata, waits for RPCS3's Utilities
-> Dump Guest Memory action to create the sparse indexed backing store, then
continues only to 0x11486c to record the command-arena after-state.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any


CHECKPOINT_PC = 0x00113584
CHECKPOINT_END_PC = 0x0011486C
CHECKPOINT_INSTRUCTION = bytes.fromhex("a23800b0")  # lhz r17, 0xb0(r24)
HOUSE_GMD = 0x429D8A00
COMMAND_ARENA_SLOT = 0x0163B980


class RspError(RuntimeError):
    pass


class RspClient:
    def __init__(self, host: str, port: int, timeout: float):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    @staticmethod
    def _checksum(payload: bytes) -> bytes:
        return f"{sum(payload) & 0xff:02x}".encode("ascii")

    def _read_exact(self, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            chunk = self.sock.recv(size - len(chunks))
            if not chunk:
                raise RspError("RPCS3 GDB connection closed")
            chunks.extend(chunk)
        return bytes(chunks)

    def _recv_packet(self) -> str:
        while True:
            char = self._read_exact(1)
            if char == b"$":
                break
            if char in (b"+", b"-"):
                continue
        payload = bytearray()
        escaped = False
        while True:
            char = self._read_exact(1)
            if not escaped and char == b"#":
                break
            if not escaped and char == b"}":
                escaped = True
                continue
            if escaped:
                char = bytes((char[0] ^ 0x20,))
                escaped = False
            payload.extend(char)
        expected = self._read_exact(2)
        if self._checksum(bytes(payload)).lower() != expected.lower():
            self.sock.sendall(b"-")
            raise RspError("bad checksum from RPCS3 GDB server")
        self.sock.sendall(b"+")
        return payload.decode("ascii", errors="replace")

    def command(self, payload: str, *, wait: bool = True) -> str:
        raw = payload.encode("ascii")
        packet = b"$" + raw + b"#" + self._checksum(raw)
        self.sock.sendall(packet)
        ack = self._read_exact(1)
        if ack != b"+":
            raise RspError(f"RPCS3 rejected GDB packet {payload!r}: {ack!r}")
        return self._recv_packet() if wait else ""

    def read_memory(self, address: int, size: int) -> bytes:
        output = bytearray()
        while len(output) < size:
            chunk = min(0x800, size - len(output))
            response = self.command(f"m{address + len(output):x},{chunk:x}")
            if response.startswith("E"):
                raise RspError(
                    f"memory read failed at 0x{address + len(output):08x}: {response}"
                )
            data = bytes.fromhex(response)
            if not data:
                raise RspError(f"empty memory read at 0x{address + len(output):08x}")
            output.extend(data)
        return bytes(output[:size])

    def read32(self, address: int) -> int:
        return struct.unpack(">I", self.read_memory(address, 4))[0]

    def read64(self, address: int) -> int:
        return struct.unpack(">Q", self.read_memory(address, 8))[0]

    def read_register(self, register_id: int) -> int:
        response = self.command(f"p{register_id:x}")
        if not response or response.startswith("E") or "x" in response.lower():
            raise RspError(f"register {register_id} unavailable: {response!r}")
        return int(response, 16)

    def threads(self) -> list[int]:
        response = self.command("qfThreadInfo")
        if not response.startswith("m"):
            return []
        body = response[1:]
        if body.endswith("l"):
            body = body[:-1]
        return [int(item, 16) for item in body.split(",") if item]

    def select_general_thread(self, thread_id: int) -> None:
        response = self.command(f"Hg{thread_id:x}")
        if response != "OK":
            raise RspError(f"could not select PPU thread 0x{thread_id:x}: {response}")

    def select_continue_any(self) -> None:
        response = self.command("Hc-1")
        if response != "OK":
            raise RspError(f"could not select continue thread: {response}")

    def add_breakpoint(self, pc: int) -> None:
        response = self.command(f"Z0,{pc:x},4")
        if response != "OK":
            raise RspError(f"could not add breakpoint 0x{pc:08x}: {response}")

    def remove_breakpoint(self, pc: int) -> None:
        response = self.command(f"z0,{pc:x},4")
        if response not in ("OK", ""):
            raise RspError(f"could not remove breakpoint 0x{pc:08x}: {response}")

    def continue_and_wait(self) -> str:
        return self.command("vCont;c")

    def kill(self) -> None:
        try:
            self.command("k", wait=False)
        except (OSError, RspError):
            pass


def connect_when_game_is_loaded(
    host: str,
    port: int,
    socket_timeout: float,
    load_timeout: float,
) -> RspClient:
    deadline = time.monotonic() + load_timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        client: RspClient | None = None
        try:
            client = RspClient(host, port, socket_timeout)
            instruction = client.read_memory(CHECKPOINT_PC, 4)
            if instruction != CHECKPOINT_INSTRUCTION:
                raise RspError(
                    "checkpoint instruction mismatch at "
                    f"0x{CHECKPOINT_PC:08x}: expected "
                    f"{CHECKPOINT_INSTRUCTION.hex()}, got {instruction.hex()}"
                )
            return client
        except (OSError, RspError) as exc:
            last_error = exc
            if client is not None:
                try:
                    client.sock.close()
                except OSError:
                    pass
            time.sleep(0.25)
    raise RspError(
        "RPCS3 did not expose the expected game code before the load timeout; "
        f"last error: {last_error}"
    )


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def register_snapshot(client: RspClient, thread_id: int) -> dict[str, Any]:
    client.select_general_thread(thread_id)
    registers = {f"r{i}": f"0x{client.read_register(i):016x}" for i in range(32)}
    return {
        "pc": f"0x{client.read_register(64):08x}",
        "cr": f"0x{client.read_register(66):08x}",
        "lr": f"0x{client.read_register(67):016x}",
        "ctr": f"0x{client.read_register(68):016x}",
        # RPCS3's GDB server currently exposes XER as unknown bytes.
        "xer": None,
        "registers": registers,
    }


def command_arena_state(client: RspClient) -> dict[str, Any]:
    arena = client.read32(COMMAND_ARENA_SLOT)
    result: dict[str, Any] = {"pointer": f"0x{arena:08x}"}
    if arena:
        result.update(
            {
                "head": f"0x{client.read32(arena + 0x00):08x}",
                "end": f"0x{client.read32(arena + 0x04):08x}",
                "count": client.read32(arena + 0x1C),
            }
        )
    return result


def find_new_dump(parent: Path, existing: set[Path], timeout: float) -> Path:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = {
            item
            for item in parent.glob("*_guest_memory_*")
            if item.is_dir() and (item / "manifest.json").exists()
        }
        new = sorted(candidates - existing, key=lambda path: path.stat().st_mtime)
        if new:
            return new[-1]
        time.sleep(0.5)
    raise TimeoutError(
        f"no completed sparse guest-memory dump appeared in {parent} "
        f"within {timeout:.0f}s"
    )


def write_metadata(path: Path, metadata: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def run_capture(args: argparse.Namespace) -> int:
    dump_parent = Path(args.dump_parent).resolve()
    dump_parent.mkdir(parents=True, exist_ok=True)
    existing = {
        item
        for item in dump_parent.glob("*_guest_memory_*")
        if item.is_dir() and (item / "manifest.json").exists()
    }

    client = connect_when_game_is_loaded(
        args.host,
        args.port,
        args.socket_timeout,
        args.load_timeout,
    )
    try:
        threads = client.threads()
        if not threads:
            raise RspError("RPCS3 reported no PPU threads")
        client.select_continue_any()
        client.add_breakpoint(CHECKPOINT_PC)
        client.add_breakpoint(CHECKPOINT_END_PC)

        paused_snapshot: tuple[int, dict[str, Any]] | None = None
        if args.adopt_paused:
            for thread_id in threads:
                try:
                    snapshot = register_snapshot(client, thread_id)
                except RspError:
                    continue
                if int(snapshot["pc"], 0) == CHECKPOINT_PC:
                    paused_snapshot = (thread_id, snapshot)
                    break
            if paused_snapshot is None:
                raise RspError(
                    "no paused PPU thread is currently at "
                    f"0x{CHECKPOINT_PC:08x}"
                )

        while True:
            adopted = paused_snapshot is not None
            if adopted:
                thread_id, snapshot = paused_snapshot
                paused_snapshot = None
            else:
                stop = client.continue_and_wait()
                if not stop.startswith("S"):
                    raise RspError(f"unexpected RPCS3 stop reply: {stop!r}")
                # The server selects the thread that caused the stop. qC exposes it.
                current = client.command("qC")
                if not current.startswith("QC"):
                    raise RspError(
                        f"RPCS3 did not report the stopped PPU thread: {current!r}"
                    )
                thread_id = int(current[2:], 16)
                snapshot = register_snapshot(client, thread_id)
            pc = int(snapshot["pc"], 0)
            if pc != CHECKPOINT_PC:
                if adopted:
                    raise RspError(
                        f"adopted thread moved to unexpected PC 0x{pc:08x}"
                    )
                continue
            r24 = int(snapshot["registers"]["r24"], 0) & 0xFFFFFFFF
            actual_gmd = client.read32(r24 + 4) if r24 else 0
            object_matches = r24 and (
                (args.adopt_paused and args.adopt_any_object)
                or actual_gmd == HOUSE_GMD
            )
            if not object_matches:
                if adopted:
                    raise RspError(
                        "paused checkpoint is not the orphanage house: "
                        f"r24=0x{r24:08x}, object_gmd=0x{actual_gmd:08x}"
                    )
                continue
            r1 = int(snapshot["registers"]["r1"], 0) & 0xFFFFFFFF
            pass_index = client.read64(r1 + 0xC0) & 0xFFFFFFFF
            pass_matches = (
                args.adopt_paused and args.adopt_any_pass
            ) or pass_index == args.pass_index
            if not pass_matches:
                if adopted:
                    raise RspError(
                        f"paused checkpoint has pass {pass_index}, "
                        f"expected {args.pass_index}"
                    )
                continue

            metadata: dict[str, Any] = {
                "schema": 1,
                "engine": "rpcs3",
                "pc": snapshot["pc"],
                "thread_id": f"0x{thread_id:08x}",
                "registers": snapshot["registers"],
                "cr": snapshot["cr"],
                "lr": snapshot["lr"],
                "ctr": snapshot["ctr"],
                "frame": {
                    "captured_utc": utc_now(),
                    "pass_index": pass_index,
                    "object_index": client.read32(r1 + 0xA4),
                    "scene_root": f"0x{client.read64(r1 + 0xE0) & 0xffffffff:08x}",
                    "object_offset": client.read64(r1 + 0xB8) & 0xFFFFFFFF,
                },
                "command_arena_before": command_arena_state(client),
                "status": "paused_waiting_for_sparse_dump",
            }
            write_metadata(Path(args.metadata), metadata)
            print(
                "RPCS3_STAGE_CHECKPOINT_PAUSED "
                f"pc=0x{CHECKPOINT_PC:08x} tid=0x{thread_id:08x} "
                f"r24=0x{r24:08x} pass={pass_index}",
                flush=True,
            )
            Path(args.ready_marker).write_text(
                json.dumps(
                    {
                        "pc": f"0x{CHECKPOINT_PC:08x}",
                        "thread_id": f"0x{thread_id:08x}",
                        "r24": f"0x{r24:08x}",
                        "object_gmd": f"0x{actual_gmd:08x}",
                        "pass_index": pass_index,
                        "metadata": str(Path(args.metadata).resolve()),
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )

            dump_dir = find_new_dump(dump_parent, existing, args.dump_timeout)
            metadata["dump_dir"] = str(dump_dir)
            metadata["dump_manifest"] = str(dump_dir / "manifest.json")
            metadata["status"] = "sparse_dump_complete"
            write_metadata(Path(args.metadata), metadata)
            print(f"RPCS3_SPARSE_DUMP_COMPLETE {dump_dir}", flush=True)

            client.remove_breakpoint(CHECKPOINT_PC)
            while True:
                stop = client.continue_and_wait()
                if not stop.startswith("S"):
                    raise RspError(f"unexpected post-call stop reply: {stop!r}")
                current = client.command("qC")
                thread_id_after = int(current[2:], 16)
                after = register_snapshot(client, thread_id_after)
                if int(after["pc"], 0) != CHECKPOINT_END_PC:
                    continue
                after_r24 = int(after["registers"]["r24"], 0) & 0xFFFFFFFF
                if after_r24 != r24:
                    continue
                metadata["command_arena_after"] = command_arena_state(client)
                metadata["end_pc"] = after["pc"]
                metadata["status"] = "complete"
                write_metadata(Path(args.metadata), metadata)
                print("RPCS3_STAGE_CHECKPOINT_COMPLETE", flush=True)
                break
            client.remove_breakpoint(CHECKPOINT_END_PC)
            if args.stop:
                client.kill()
            return 0
    finally:
        try:
            client.sock.close()
        except OSError:
            pass


def self_test(_: argparse.Namespace) -> int:
    assert RspClient._checksum(b"qSupported") == b"37"
    assert RspClient._checksum(b"m113584,4") == f"{sum(b'm113584,4') & 255:02x}".encode()
    print("rpcs3_stage_checkpoint self-test: PASS")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    capture = sub.add_parser("capture")
    capture.add_argument("--host", default="127.0.0.1")
    capture.add_argument("--port", type=int, default=2345)
    capture.add_argument("--socket-timeout", type=float, default=3600)
    capture.add_argument("--dump-timeout", type=float, default=900)
    capture.add_argument("--load-timeout", type=float, default=300)
    capture.add_argument(
        "--adopt-paused",
        action="store_true",
        help="adopt an existing UI/debugger pause at the checkpoint without resuming",
    )
    capture.add_argument(
        "--adopt-any-object",
        action="store_true",
        help="with --adopt-paused, preserve the current non-null r24 object identity",
    )
    capture.add_argument(
        "--adopt-any-pass",
        action="store_true",
        help="with --adopt-paused, preserve the current render pass",
    )
    capture.add_argument("--dump-parent", required=True)
    capture.add_argument("--metadata", required=True)
    capture.add_argument("--ready-marker", required=True)
    capture.add_argument("--pass-index", type=int, default=0)
    capture.add_argument("--stop", action=argparse.BooleanOptionalAction, default=True)
    capture.set_defaults(func=run_capture)
    test = sub.add_parser("self-test")
    test.set_defaults(func=self_test)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
