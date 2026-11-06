from dataclasses import dataclass
import json
from pathlib import Path

REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eflags")

@dataclass
class Case:
    data: dict

    @classmethod
    def load(cls, path):
        return cls(json.loads(Path(path).read_text(encoding="utf-8")))

    def save(self, path):
        Path(path).write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def validate(self):
        required = ("name", "code", "entry_eip", "stop", "state", "memory")
        missing = [key for key in required if key not in self.data]
        if missing:
            raise ValueError("missing case fields: " + ", ".join(missing))
        missing = [key for key in REGS if key not in self.data["state"]]
        if missing:
            raise ValueError("missing registers: " + ", ".join(missing))
        if not isinstance(self.data["code"], str) or len(self.data["code"]) % 2:
            raise ValueError("code must be an even-length hex string")
        return self
