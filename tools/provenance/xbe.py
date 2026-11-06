import argparse
import json
from tools.disasm.loader import load_image
from tools.recomp.disasm import Disassembler
from .analyze import analyze

def _operand(op):
    if op.type == "reg":
        return op.reg
    if op.type == "imm":
        return op.imm
    if op.type == "mem":
        if not op.mem_base and not op.mem_index:
            return {"kind": "global", "address": op.mem_disp & 0xFFFFFFFF}
        return {"kind": "mem", "base": op.mem_base, "index": op.mem_index, "disp": op.mem_disp}
    return None

def report(xbe, functions, address):
    image = load_image(xbe)
    entries = json.loads(open(functions, encoding="utf-8").read())
    function = next(item for item in entries if int(item["start"], 0) == address)
    end = int(function["end"], 0)
    raw = image.read_bytes_at_va(address, end - address)
    instructions = Disassembler().disassemble_function(raw, address, end)
    normalized = [{"address": item.address, "mnemonic": item.mnemonic,
                   "operands": [_operand(op) for op in item.operands]} for item in instructions]
    state, events = analyze(normalized)
    return {"function": function, "registers": {key: str(value) for key, value in state.items()}, "events": events}

def main():
    parser = argparse.ArgumentParser(description="Conservative provenance report for one XBE function")
    parser.add_argument("--xbe", required=True)
    parser.add_argument("--functions", required=True)
    parser.add_argument("--func", required=True, type=lambda value: int(value, 0))
    args = parser.parse_args()
    print(json.dumps(report(args.xbe, args.functions, args.func), indent=2))

if __name__ == "__main__":
    main()
