from dataclasses import dataclass

@dataclass
class Divergence:
    index: int
    reason: str
    oracle: dict
    recomp: dict

def _diff(a, b, prefix=""):
    for key in sorted(set(a) | set(b)):
        left, right = a.get(key), b.get(key)
        name = f"{prefix}.{key}" if prefix else key
        if isinstance(left, dict) and isinstance(right, dict):
            nested = _diff(left, right, name)
            if nested:
                return nested
        elif isinstance(left, list) and isinstance(right, list):
            for index, values in enumerate(zip(left, right)):
                if values[0] != values[1]:
                    return _diff({"value": values[0]}, {"value": values[1]}, f"{name}[{index}]") or f"{name}[{index}]"
            if len(left) != len(right):
                return f"{name}.length"
        elif left != right:
            return name
    return None

def first_divergence(oracle, recomp):
    for index, (left, right) in enumerate(zip(oracle, recomp)):
        field = _diff(left, right)
        if field:
            return Divergence(index, field, left, right)
    if len(oracle) != len(recomp):
        index = min(len(oracle), len(recomp))
        return Divergence(index, "trace length", oracle[index:] if index < len(oracle) else {}, recomp[index:] if index < len(recomp) else {})
    return None

def format_divergence(divergence):
    if not divergence:
        return "MATCH"
    o, r = divergence.oracle, divergence.recomp
    eip = o.get("eip", r.get("eip", 0))
    key = divergence.reason.rsplit(".", 1)[-1]
    return (f"FIRST DIVERGENCE\ncheckpoint: {divergence.index}\n"
            f"guest EIP: 0x{eip:08X}\nfield: {divergence.reason}\n"
            f"oracle: {o.get(key, o)!r}\n"
            f"recomp: {r.get(key, r)!r}")
