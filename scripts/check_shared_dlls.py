# /// script
# requires-python = ">=3.11"
# dependencies = ["pefile>=2023.2.7"]
# ///
"""Checks that staged UE DLLs still load next to Houdini's copies of the same DLLs.

Windows loads a DLL name once per process: when Houdini has already loaded its tbb12.dll, embree4.dll binds to that
one, not to the newer copy staged beside the backend. Fails when a staged DLL imports a symbol that Houdini's DLL of
the same name does not export.

    uv run scripts/check_shared_dlls.py <staged runtime folder> <$HFS/bin>
"""

import sys
from pathlib import Path

import pefile

DIRECTORIES = [
    pefile.DIRECTORY_ENTRY[name]
    for name in (
        "IMAGE_DIRECTORY_ENTRY_IMPORT",
        "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT",
        "IMAGE_DIRECTORY_ENTRY_EXPORT",
    )
]


def load(path):
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=DIRECTORIES)
    return pe


def imports(pe):
    """{dll name (lower case): {symbol names}}, ordinal imports left out."""
    found = {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) + getattr(
        pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []
    ):
        names = found.setdefault(entry.dll.decode().lower(), set())
        names.update(i.name.decode() for i in entry.imports if i.name)
    return found


def exports(pe):
    directory = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    return (
        {s.name.decode() for s in directory.symbols if s.name}
        if directory
        else set()
    )


def main(staged, houdini_bin):
    staged = list(Path(staged).rglob("*.dll"))
    shared = {
        p.name.lower(): Path(houdini_bin) / p.name
        for p in staged
        if (Path(houdini_bin) / p.name).exists()
    }
    houdini_exports = {
        name: exports(load(path)) for name, path in shared.items()
    }
    missing = []
    for dll in staged:
        for name, symbols in imports(load(dll)).items():
            if name in houdini_exports:
                missing += [
                    f"{dll.name} imports {name}!{symbol}, which Houdini's {shared[name]} does not export"
                    for symbol in sorted(symbols - houdini_exports[name])
                ]
    print(
        f"{len(staged)} staged DLLs; shared with Houdini: {', '.join(sorted(shared)) or 'none'}"
    )
    print(
        "\n".join(missing)
        or "Every import from a shared DLL resolves in Houdini's copy."
    )
    return 1 if missing else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
