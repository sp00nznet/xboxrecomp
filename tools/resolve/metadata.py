import json
from pathlib import Path

FIELDS = ("guest_start", "guest_end", "symbol", "xbe_section", "xbe_hash")

def load(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    entries = data.get("functions", data)
    if not isinstance(entries, list):
        raise ValueError("resolver metadata must contain a functions list")
    return entries

def resolve(entries, address):
    def value(item, key):
        raw = item[key]
        return int(raw, 0) if isinstance(raw, str) else raw
    exact = [item for item in entries if value(item, "guest_start") <= address < value(item, "guest_end")]
    if exact:
        item = exact[0]
        return {"entry": item, "secondary": address != value(item, "guest_start"), "classification": item.get("classification", "unknown")}
    nearest = min(entries, key=lambda item: min(abs(address-value(item, "guest_start")), abs(address-value(item, "guest_end"))), default=None)
    return {"entry": None, "nearest": nearest, "classification": "unknown"}
