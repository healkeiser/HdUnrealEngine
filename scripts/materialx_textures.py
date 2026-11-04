"""Filesystem UDIM discovery shared by preparation and runtime validation."""

import os
import re
from pathlib import Path

# Directory reads, keyed by the directory's modification time: adding, removing or renaming an entry there changes
# it, so a cached answer is never stale. A scene's materials name the same files many times (NVIDIA's Da Vinci
# Workshop: 20,000 texture inputs, 8,000 files), and each was resolved and its UDIM folder listed again.
_DIRECTORY_CACHE_LIMIT = 1 << 16
_udim_cache = {}
_resolve_cache = {}


def _directory_stamp(directory):
    try:
        return os.stat(directory).st_mtime_ns
    except OSError:
        return None


def resolved(path):
    """Path(path).resolve() as a string, cached while its directory is unchanged."""
    path = str(path)
    key = (path, _directory_stamp(os.path.dirname(path)))
    result = _resolve_cache.get(key)
    if result is None:
        if len(_resolve_cache) >= _DIRECTORY_CACHE_LIMIT:
            _resolve_cache.clear()
        result = _resolve_cache[key] = str(Path(path).resolve())
    return result


def udim_tiles(pattern):
    """The tiles of a UDIM pattern {index: resolved file}, cached while its directory is unchanged."""
    key = (str(pattern), _directory_stamp(os.path.dirname(str(pattern))))
    cached = _udim_cache.get(key)
    if cached is None:
        if len(_udim_cache) >= _DIRECTORY_CACHE_LIMIT:
            _udim_cache.clear()
        try:
            cached = _udim_cache[key] = (_udim_tiles(pattern), None)
        except ValueError as error:
            cached = _udim_cache[key] = (None, error)
    tiles, error = cached
    if error:
        raise error
    return dict(tiles)


def _udim_tiles(pattern):
    path = Path(pattern)
    # Matches UE 5.8's BaseName.####.ext / BaseName_####.ext naming rule.
    if path.name.count("<UDIM>") != 1 or not re.fullmatch(
        r".+[._]<UDIM>\.[^.]+", path.name
    ):
        raise ValueError(
            f"UDIM needs BaseName.<UDIM>.ext or BaseName_<UDIM>.ext: {pattern}"
        )
    prefix, suffix = path.name.split("<UDIM>")
    expression = re.compile(
        re.escape(prefix) + r"(\d{4})" + re.escape(suffix), re.IGNORECASE
    )
    tiles = {}
    if path.parent.is_dir():
        for file in path.parent.iterdir():
            match = expression.fullmatch(file.name)
            if not match or not file.is_file():
                continue
            index = int(match[1])
            if not 1001 <= index <= 1910:
                raise ValueError(
                    f"UDIM index is outside UE's 1001..1910 range: {file}"
                )
            if index in tiles:
                raise ValueError(f"Duplicate UDIM index {index}: {pattern}")
            tiles[index] = resolved(file)
    if not tiles:
        raise ValueError(f"UDIM has no tiles: {pattern}")
    # UE discards offsets when importing a single UDIM image as ordinary 2D.
    if len(tiles) == 1 and 1001 not in tiles:
        raise ValueError(
            f"A single off-origin UDIM tile loses its offset in UE; provide another tile: {pattern}"
        )
    return dict(sorted(tiles.items()))


def validate_texture_sets(sets):
    for record in sets:
        actual = {str(k): v for k, v in udim_tiles(record["pattern"]).items()}
        if actual != record["tiles"]:
            raise ValueError(
                f"UDIM tile membership changed; prepare/import/cook again: {record['pattern']}"
            )


def file_identity(path):
    """A texture dependency's identity: its size and modification time, as hdUE's texture caches key files
    (Scene.cpp CachePath). Hashing the bytes read every texture on Houdini's UI thread: NVIDIA's Da Vinci Workshop
    converted to MaterialX reads 60 GB of tiles, and Houdini stopped responding for minutes on each load.
    """
    stat = Path(path).stat()
    return f"stat:{stat.st_size}:{stat.st_mtime_ns}"


def file_unchanged(path, expected):
    """Whether a recorded dependency still holds: a file_identity, or a SHA-256 of the bytes (older records)."""
    path = Path(path)
    if not path.is_file():
        return False
    if expected.startswith("stat:"):
        return file_identity(path) == expected
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest() == expected
