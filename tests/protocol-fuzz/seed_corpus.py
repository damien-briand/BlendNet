"""Create framing/schema seeds for all protocol v1 types, including hostile JSON."""

import pathlib
import re
import struct
import sys


def main():
    corpus = pathlib.Path(sys.argv[1])
    corpus.mkdir(parents=True, exist_ok=True)
    fixtures = pathlib.Path(__file__).parents[1] / "unit" / "ProtocolCodecTests.cpp"
    # These are independent known-valid protocol examples already used by the tests.
    payloads = re.findall(r'R"\((.*?)\)"', fixtures.read_text(), re.S)
    constants = (fixtures.parents[1].parent / "src/core/protocol/MessageTypes.h").read_text()
    magic = int(re.search(r'protocolMagic = (0x[0-9a-fA-F]+)', constants).group(1), 16)
    types = {name: int(number) for name, number in re.findall(r'^    (\w+) = (\d+)', constants, re.M)}
    for kind in types.values():
        for index, payload in enumerate(["{}", "[]", "null", "{", '{"a":1,"a":2}',
                                         "[" * 65 + "]" * 65, *payloads]):
            body = payload.encode()
            header = struct.pack(">HBBII", magic, 0, kind, 0, len(body))
            (corpus / f"frame-{kind:02}-{index:03}").write_bytes(header + body)
            (corpus / f"payload-{kind:02}-{index:03}").write_bytes(bytes([kind]) + body)
    (corpus / "oversized-control").write_bytes(struct.pack(">HBBII", magic, 0, types["Hello"], 0, 65537))
    (corpus / "binary").write_bytes(bytes([types["FileData"]]) + struct.pack(">I", 3) + b"job" +
                                  struct.pack(">II", 0, 3) + b"abc")
    print(f"Created {len(list(corpus.iterdir()))} seeds in {corpus}")


if __name__ == "__main__":
    main()
