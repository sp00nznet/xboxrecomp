import argparse
from .case import capture_case

def main():
    parser = argparse.ArgumentParser(description="Capture a local XBE replay case")
    parser.add_argument("--xbe", required=True)
    parser.add_argument("--func", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--analysis-json")
    parser.add_argument("--instructions", type=int)
    args = parser.parse_args()
    capture_case(args.xbe, args.func, args.state, args.out, args.analysis_json, args.instructions)

if __name__ == "__main__":
    main()
