"""Decode raw records copied from g_recomp_diff_checkpoints."""
import argparse
import json
import struct
from pathlib import Path

FIELDS = ("sequence", "eip", "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "eflags")

def decode(path):
    data = Path(path).read_bytes()
    size = struct.calcsize("<10I")
    if len(data) % size:
        raise ValueError("truncated checkpoint record")
    return [dict(zip(FIELDS, struct.unpack_from("<10I", data, offset)))
            for offset in range(0, len(data), size)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint")
    args = parser.parse_args()
    print(json.dumps(decode(args.checkpoint), indent=2))

if __name__ == "__main__":
    main()
