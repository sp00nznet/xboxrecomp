import hashlib
import json
from pathlib import Path

from tools.disasm.loader import load_image

def capture_case(xbe, function, state, output, analysis_json=None, instructions=None):
    """Build a local replay case; keep copyrighted output outside git."""
    path = Path(xbe)
    address = int(function, 0) if isinstance(function, str) else function
    image = load_image(str(path), analysis_json)
    snapshot = json.loads(Path(state).read_text(encoding="utf-8"))
    if "state" not in snapshot:
        raise ValueError("state JSON must contain a state object")
    end = snapshot.get("end_eip")
    if end is None:
        raise ValueError("state JSON must contain end_eip")
    end = int(end, 0) if isinstance(end, str) else end
    code = image.read_bytes_at_va(address, end - address)
    if not code:
        raise ValueError("function range is not backed by XBE bytes")
    metadata = {"xbe": str(path), "xbe_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "function": f"0x{address:08X}", "copyrighted_bytes": True, "local_only": True}
    case = {"name": f"capture-{address:08X}", "code": code.hex(), "entry_eip": address,
            "stop": snapshot.get("stop", {"ret": True, "instructions": instructions or 100000}),
            "state": snapshot["state"], "memory": snapshot.get("memory", []),
            "calls": snapshot.get("calls", snapshot.get("external_calls", [])), "capture": metadata}
    out = Path(output)
    out.mkdir(parents=True, exist_ok=True)
    (out / "case.json").write_text(json.dumps(case, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (out / "capture.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return out / "case.json"

capture_metadata = capture_case
