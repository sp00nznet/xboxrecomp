"""Small runner boundary; Unicorn is optional and never silently substituted."""
import json
from pathlib import Path

class RunnerUnavailable(RuntimeError):
    pass

def run_unicorn(case, *, trace=True):
    try:
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
        from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
            UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP,
            UC_X86_REG_ESP, UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_MXCSR,
            UC_X86_REG_XMM0, UC_X86_REG_ST0)
    except ImportError as error:
        raise RunnerUnavailable("install unicorn to run the x86 oracle") from error
    state = case.data["state"]
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pages = {}
    mapped_pages = set()
    for page in case.data["memory"]:
        address = int(page["address"], 0) if isinstance(page["address"], str) else page["address"]
        blob = bytes.fromhex(page["data"])
        size = (len(blob) + 0xFFF) & ~0xFFF
        uc.mem_map(address & ~0xFFF, max(size, 0x1000))
        mapped_pages.add(address & ~0xFFF)
        uc.mem_write(address, blob)
        pages[address] = len(blob)
    code = bytes.fromhex(case.data["code"])
    entry = int(case.data["entry_eip"], 0) if isinstance(case.data["entry_eip"], str) else case.data["entry_eip"]
    uc.mem_map(entry & ~0xFFF, max(0x1000, (len(code) + (entry & 0xFFF) + 0xFFF) & ~0xFFF))
    mapped_pages.add(entry & ~0xFFF)
    uc.mem_write(entry, code)
    uc.reg_write(UC_X86_REG_EIP, entry)
    transcripts = {}
    for item in case.data.get("calls", []):
        target = item.get("target")
        if target is None:
            continue
        target = int(target, 0) if isinstance(target, str) else target
        transcripts.setdefault(target, []).append(item)
        page = target & ~0xFFF
        if page not in mapped_pages:
            uc.mem_map(page, 0x1000)
            mapped_pages.add(page)
        uc.mem_write(target, b"\xC3")
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        instruction_text = {item.address: f"{item.mnemonic} {item.op_str}".rstrip()
                            for item in decoder.disasm(code, entry)}
    except ImportError:
        instruction_text = {}
    initial_memory = []
    for page in case.data["memory"]:
        address = int(page["address"], 0) if isinstance(page["address"], str) else page["address"]
        blob = bytes.fromhex(page["data"])
        initial_memory.append((address, blob))
    regs = {"eax": UC_X86_REG_EAX, "ebx": UC_X86_REG_EBX, "ecx": UC_X86_REG_ECX,
            "edx": UC_X86_REG_EDX, "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
            "ebp": UC_X86_REG_EBP, "esp": UC_X86_REG_ESP, "eflags": UC_X86_REG_EFLAGS}
    for name, reg in regs.items():
        uc.reg_write(reg, int(state[name], 0) if isinstance(state[name], str) else state[name])
    for i in range(8):
        if isinstance(state.get("sse"), dict) and f"xmm{i}" in state["sse"]:
            uc.reg_write(UC_X86_REG_XMM0 + i, state["sse"][f"xmm{i}"])
        if isinstance(state.get("x87"), list) and i < len(state["x87"]):
            uc.reg_write(UC_X86_REG_ST0 + i, state["x87"][i])
    if "mxcsr" in state:
        uc.reg_write(UC_X86_REG_MXCSR, state["mxcsr"])
    checkpoints = []
    writes = []
    calls = []
    stub_called = False
    transcript_index = {target: 0 for target in transcripts}
    def snapshot():
        eip = uc.reg_read(UC_X86_REG_EIP)
        result = {"eip": eip, "instruction": instruction_text.get(eip),
                  **{name: uc.reg_read(reg) for name, reg in regs.items()}}
        result["sse"] = {f"xmm{i}": uc.reg_read(UC_X86_REG_XMM0 + i) for i in range(8)}
        result["mxcsr"] = uc.reg_read(UC_X86_REG_MXCSR)
        result["x87"] = [uc.reg_read(UC_X86_REG_ST0 + i) for i in range(8)]
        return result
    stop = case.data["stop"]
    limit = int(stop.get("instructions", 1000000))
    stop_eip = stop.get("eip")
    if isinstance(stop_eip, str):
        stop_eip = int(stop_eip, 0)
    def hook(_, address, size, __):
        nonlocal stub_called
        if address in transcripts:
            index = transcript_index[address]
            record = transcripts[address][min(index, len(transcripts[address]) - 1)]
            transcript_index[address] = index + 1
            calls.append({"target": address, "sequence": len(calls)})
            for name in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "eflags"):
                if name in record:
                    uc.reg_write(regs[name], int(record[name], 0) if isinstance(record[name], str) else record[name])
            for write in record.get("writes", []):
                write_address = int(write["address"], 0) if isinstance(write["address"], str) else write["address"]
                raw_value = write.get("value", 0)
                value = int(raw_value, 0) if isinstance(raw_value, str) else raw_value
                width = int(write.get("size", 4))
                uc.mem_write(write_address, value.to_bytes(width, "little"))
            return_address = int.from_bytes(bytes(uc.mem_read(uc.reg_read(UC_X86_REG_ESP), 4)), "little")
            uc.reg_write(UC_X86_REG_ESP, uc.reg_read(UC_X86_REG_ESP) + 4)
            uc.reg_write(UC_X86_REG_EIP, return_address)
            stub_called = True
            uc.emu_stop()
            return
        if trace:
            checkpoints.append(snapshot())
        if stop_eip is not None and address == stop_eip:
            uc.emu_stop()
        if stop.get("ret") and 0 <= address - entry < len(code) and code[address - entry] in (0xC2, 0xC3):
            uc.emu_stop()
    from unicorn import UC_HOOK_CODE
    uc.hook_add(UC_HOOK_CODE, hook)
    from unicorn import UC_HOOK_MEM_WRITE
    uc.hook_add(UC_HOOK_MEM_WRITE, lambda _, __, address, size, value, ___: writes.append({"address": address, "size": size, "value": value}))
    try:
        remaining = limit
        while remaining > 0:
            stub_called = False
            before = len(checkpoints)
            uc.emu_start(uc.reg_read(UC_X86_REG_EIP), 0, count=remaining)
            consumed = max(1, len(checkpoints) - before)
            remaining -= consumed
            if not stub_called:
                break
    except Exception as error:
        failed = snapshot()
        failed["exception"] = {"type": type(error).__name__, "message": str(error)}
        checkpoints.append(failed)
    if checkpoints:
        dirty = []
        for address, before in initial_memory:
            after = bytes(uc.mem_read(address, len(before)))
            if after != before:
                for offset, (old, new) in enumerate(zip(before, after)):
                    if old != new:
                        dirty.append({"address": address + offset, "old": old, "new": new})
        for item in checkpoints:
            item["writes"] = list(writes)
            item["dirty_memory"] = dirty
            item["calls"] = list(case.data.get("calls", []))
            item["call_events"] = list(calls)
    return checkpoints

def save_trace(path, trace):
    Path(path).write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
