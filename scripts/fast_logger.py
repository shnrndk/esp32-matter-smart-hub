"""Reduce build time for the generated CLI diagnostic logger only."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
path = root / ".toolchains/esp-matter/connectedhomeip/connectedhomeip/out/host/obj/examples/chip-tool/chip-tool-utils.ninja"
lines = path.read_text().splitlines(keepends=True)
flags = next(line.strip().split(" = ", 1)[1] for line in lines if line.startswith("cflags_cc = "))
for index, line in enumerate(lines):
    if line.startswith("build ") and ".DataModelLogger.cpp.o: cxx " in line:
        override = "  cflags_cc = " + flags + " -O0\n"
        if lines[index + 1].startswith("  cflags_cc = "):
            lines[index + 1] = override
        else:
            lines.insert(index + 1, override)
        break
else:
    raise SystemExit("SDK logger target changed; review the generated Ninja file before rebuilding.")
path.write_text("".join(lines))
