import argparse
import struct

RECORD = struct.Struct("<7I")

def decode(path):
    with open(path, "rb") as stream:
        while blob := stream.read(RECORD.size):
            if len(blob) != RECORD.size:
                raise ValueError("truncated trace record")
            yield dict(zip(("sequence", "thread", "eip", "type", "arg0", "arg1", "arg2"), RECORD.unpack(blob)))

def main():
    parser = argparse.ArgumentParser(description="Decode XboxRecomp binary trace records")
    parser.add_argument("trace")
    args = parser.parse_args()
    for record in decode(args.trace):
        print("{sequence:08d} thread={thread} eip=0x{eip:08X} type={type} arg0=0x{arg0:08X} arg1=0x{arg1:08X} arg2=0x{arg2:08X}".format(**record))

if __name__ == "__main__":
    main()
