from dataclasses import dataclass

UNKNOWN = "UNKNOWN"

@dataclass(frozen=True)
class Value:
    kind: str
    args: tuple = ()
    def __str__(self):
        return self.kind if not self.args else f"{self.kind}({','.join(map(str, self.args))})"

def join(left, right):
    return left if left == right else Value(UNKNOWN)

def analyze(instructions, initial=None):
    """Analyze normalized instructions: {address,mnemonic,operands}.

    Operands use register names or memory tuples (base, displacement). Unknown
    instructions invalidate their destination; ambiguous joins must be merged
    by the caller with ``join``.
    """
    names = initial.keys() if isinstance(initial, dict) else (initial or ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"))
    state = {name: Value(UNKNOWN) for name in names}
    stack = {}
    events = []
    def read(operand):
        if isinstance(operand, str):
            return state.get(operand, Value(UNKNOWN))
        if isinstance(operand, dict) and operand.get("kind") == "global":
            return Value("GLOBAL", (hex(operand["address"]),))
        if isinstance(operand, dict) and operand.get("kind") == "mem":
            if operand.get("base") == "esp" and not operand.get("index") and operand.get("disp", 0) >= 4:
                return Value("STACK_ARG", (f"+0x{operand['disp']:X}",))
            base = state.get(operand.get("base"), Value(UNKNOWN))
            return Value("LOAD", (str(base), operand.get("disp", 0)))
        return Value(UNKNOWN)
    for insn in instructions:
        op = insn.get("operands", [])
        mnemonic = insn["mnemonic"].lower()
        if mnemonic in ("mov", "lea", "movsx", "movzx") and len(op) == 2 and isinstance(op[0], str):
            value = read(op[1])
            if mnemonic == "lea" and isinstance(op[1], dict) and op[1].get("kind") == "mem":
                value = Value("DERIVED", (str(state.get(op[1].get("base"), Value(UNKNOWN))), op[1].get("disp", 0)))
            state[op[0]] = value
        elif mnemonic in ("add", "sub") and len(op) == 2 and isinstance(op[0], str):
            value = read(op[1])
            if isinstance(value, (int, str)) or isinstance(op[1], int):
                state[op[0]] = Value("DERIVED", (str(state.get(op[0], Value(UNKNOWN))), op[1]))
            else:
                state[op[0]] = Value(UNKNOWN)
        elif mnemonic in ("push", "pop"):
            events.append({"address": insn["address"], "kind": mnemonic, "value": str(read(op[0])) if op else UNKNOWN})
            if mnemonic == "push" and op:
                stack[len(stack)] = read(op[0])
            elif mnemonic == "pop" and op and isinstance(op[0], str) and stack:
                state[op[0]] = stack.pop(max(stack))
        elif mnemonic in ("call", "jmp") and op:
            target = Value("CONST", (hex(op[0]),)) if isinstance(op[0], int) else read(op[0])
            kind = "direct-control" if isinstance(op[0], int) else "indirect-control"
            events.append({"address": insn["address"], "kind": kind, "target": str(target)})
            if mnemonic == "call" and isinstance(op[0], int):
                state["eax"] = Value("RETURN_FROM", (hex(op[0]),))
        elif op and isinstance(op[0], str):
            state[op[0]] = Value(UNKNOWN)
    return state, events
