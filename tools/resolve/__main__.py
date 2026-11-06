import argparse
from .metadata import load, resolve

def main():
    parser = argparse.ArgumentParser(description="Resolve an Xbox guest VA from metadata")
    parser.add_argument("metadata")
    parser.add_argument("address", type=lambda value: int(value, 0))
    args = parser.parse_args()
    result = resolve(load(args.metadata), args.address)
    if result["entry"]:
        entry = result["entry"]
        print(f"0x{args.address:08X}: {entry.get('symbol', '<unnamed>')} "
              f"secondary={result['secondary']} classification={result['classification']}")
    else:
        nearest = result.get("nearest")
        print(f"0x{args.address:08X}: UNKNOWN nearest={nearest.get('symbol') if nearest else None}")

if __name__ == "__main__":
    main()
