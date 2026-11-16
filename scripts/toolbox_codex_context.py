# SPDX-License-Identifier: GPL-3.0-or-later
"""Short-lived, lossless Codex context configuration editor. No network access."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import tomllib
import uuid

KEYS = ("model_context_window", "model_auto_compact_token_limit")
VALUES = (1_000_000, 900_000)


def statements(text):
    """Split only at TOML statement boundaries, never inside strings/arrays."""
    start = i = depth = 0
    quote = None
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\" and quote.startswith('"'):
                i += 2
                continue
            if text.startswith(quote, i):
                i += len(quote)
                quote = None
                continue
        elif c in "\"'":
            quote = c * 3 if text.startswith(c * 3, i) else c
            i += len(quote)
            continue
        elif c == "#":
            end = text.find("\n", i)
            i = len(text) if end < 0 else end
            continue
        elif c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        elif c == "\n" and depth == 0:
            yield start, i + 1, text[start:i + 1]
            start = i + 1
        i += 1
    if start < len(text):
        yield start, len(text), text[start:]


def leaf_path(data):
    path = []
    while isinstance(data, dict):
        if len(data) != 1:
            raise ValueError("layout")
        key, data = next(iter(data.items()))
        path.append(key)
    return tuple(path)


def assignments(text):
    table = ()
    for start, end, statement in statements(text):
        value = statement.strip()
        if not value or value.startswith("#"):
            continue
        if value.startswith("["):
            # A synthetic leaf resolves quoted/dotted table paths with tomllib itself.
            parsed = tomllib.loads(value + "\n__snow_marker__ = 0\n")
            if value.startswith("[["):
                table = ("__array_table__",)  # never a Codex profile scope
            else:
                table = leaf_path(parsed)[:-1]
            continue
        quote = None
        for pos, c in enumerate(value):
            if c in "\"'":
                if quote == c:
                    quote = None
                elif quote is None:
                    quote = c
            elif c == "=" and quote is None:
                key = leaf_path(tomllib.loads(value[:pos] + " = 0"))
                yield start, end, table + key
                break


def scope(data):
    profile = data.get("profile")
    profiles = data.get("profiles", {})
    selected = profiles.get(profile, {}) if isinstance(profile, str) else {}
    return profile, selected


def snapshot(raw):
    data = tomllib.loads(raw.decode("utf-8-sig"))
    profile, selected = scope(data)
    values = [selected.get(k, data.get(k)) for k in KEYS]
    state = "default" if all(v is None for v in values) else (
        "1m" if values[0] == VALUES[0] and values[1] in (None, VALUES[1]) else "custom")
    return {"state": state, "revision": hashlib.sha256(raw).hexdigest(),
            "profile": profile or ""}


def patch(raw, enabled):
    text = raw.decode("utf-8-sig")
    data = tomllib.loads(text)
    profile, selected = scope(data)
    scopes = [()]
    if any(k in selected for k in KEYS):
        scopes.append(("profiles", profile))
    targets = {s + (k,) for s in scopes for k in KEYS}
    spans = [(start, end) for start, end, path in assignments(text) if path in targets]
    # Preserve unrelated bytes, comments, BOM and line endings. New global keys precede tables.
    result = text
    for start, end in reversed(spans):
        result = result[:start] + result[end:]
    newline = "\r\n" if "\r\n" in text else "\n"
    if enabled:
        prefix = "".join(f"{k} = {v}{newline}" for k, v in zip(KEYS, VALUES))
        # Profile overrides were removed; inherit the global 1M preset.
        result = prefix + result
    parsed = tomllib.loads(result)
    _, selected_after = scope(parsed)
    expected = list(VALUES) if enabled else [None, None]
    if [selected_after.get(k, parsed.get(k)) for k in KEYS] != expected:
        raise ValueError("layout")  # e.g. inline profile table: never rewrite unrelated structure
    expected_data = dict(data)
    for k in KEYS:
        expected_data.pop(k, None)
    if len(scopes) > 1:
        expected_data["profiles"] = dict(data["profiles"])
        expected_data["profiles"][profile] = dict(selected)
        for k in KEYS:
            expected_data["profiles"][profile].pop(k, None)
    if enabled:
        expected_data.update(zip(KEYS, VALUES))
    if parsed != expected_data:
        raise ValueError("layout")
    return (b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b"") + result.encode("utf-8")


def config_path():
    home = os.environ.get("CODEX_HOME")
    return (Path(home).expanduser() if home else Path.home() / ".codex") / "config.toml"


def execute(action, revision=""):
    path = config_path()
    raw = path.read_bytes() if path.exists() else b""
    current = snapshot(raw)
    if action == "read":
        return dict(current, path=str(path), backup="")
    if action not in ("on", "off"):
        raise ValueError("request")
    if current["revision"] != revision:
        raise ValueError("changed")
    patched = patch(raw, action == "on")
    backup = ""
    if patched != raw:
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.is_symlink():
            raise ValueError("layout")
        # Exclude other toolbox instances; recheck immediately before atomic replacement.
        lock = path.with_name(".snow-toolbox-context.lock")
        lock_stream = lock.open("xb")
        try:
            with lock_stream:
                if (path.read_bytes() if path.exists() else b"") != raw:
                    raise ValueError("changed")
                if path.exists():
                    backup_path = path.with_name("config.toml.snow-toolbox-" + uuid.uuid4().hex + ".bak")
                    with backup_path.open("xb") as stream:
                        stream.write(raw)
                    backup = str(backup_path)
                fd, temporary = tempfile.mkstemp(prefix=".snow-context-", dir=path.parent)
                try:
                    with os.fdopen(fd, "wb") as stream:
                        stream.write(patched)
                        stream.flush()
                        os.fsync(stream.fileno())
                    if (path.read_bytes() if path.exists() else b"") != raw:
                        raise ValueError("changed")
                    os.replace(temporary, path)
                finally:
                    Path(temporary).unlink(missing_ok=True)
        finally:
            lock.unlink(missing_ok=True)
    return dict(snapshot(patched), path=str(path), backup=backup)


if __name__ == "__main__":
    try:
        result = execute(*sys.argv[1:])
    except tomllib.TOMLDecodeError:
        result = {"error": "invalid"}
    except (UnicodeError, ValueError) as error:
        result = {"error": str(error) if str(error) in ("layout", "changed", "request") else "invalid"}
    except OSError:
        result = {"error": "io"}
    print(json.dumps(result, ensure_ascii=True))
