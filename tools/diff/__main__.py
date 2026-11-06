import argparse
import json
import os
import subprocess
from .case import Case
from .compare import first_divergence, format_divergence

def main():
    parser = argparse.ArgumentParser(description="Compare bounded Xbox guest checkpoints")
    parser.add_argument("case", help="case directory or case.json")
    parser.add_argument("--oracle", required=True)
    parser.add_argument("--recomp", required=True)
    parser.add_argument("--run-oracle", action="store_true", help="run optional Unicorn oracle")
    parser.add_argument("--oracle-command", help="command that writes oracle checkpoints to --oracle")
    parser.add_argument("--recomp-command", help="command that writes recomp checkpoints to --recomp")
    args = parser.parse_args()
    path = args.case if args.case.endswith(".json") else args.case + "/case.json"
    Case.load(path).validate()
    if args.run_oracle:
        from .runner import run_unicorn, save_trace
        oracle = run_unicorn(Case.load(path).validate())
        save_trace(args.oracle, oracle)
    elif args.oracle_command:
        environment = os.environ.copy()
        environment["XBOXRECOMP_DIFF_CASE"] = path
        environment["XBOXRECOMP_DIFF_OUT"] = args.oracle
        subprocess.run(args.oracle_command, shell=True, check=True, env=environment)
        with open(args.oracle, encoding="utf-8") as stream:
            oracle = json.load(stream)
    else:
        with open(args.oracle, encoding="utf-8") as stream:
            oracle = json.load(stream)
    if args.recomp_command:
        environment = os.environ.copy()
        environment["XBOXRECOMP_DIFF_CASE"] = path
        environment["XBOXRECOMP_DIFF_OUT"] = args.recomp
        subprocess.run(args.recomp_command, shell=True, check=True, env=environment)
    with open(args.recomp, encoding="utf-8") as stream:
        recomp = json.load(stream)
    result = first_divergence(oracle, recomp)
    print(format_divergence(result))
    return 1 if result else 0

if __name__ == "__main__":
    raise SystemExit(main())
