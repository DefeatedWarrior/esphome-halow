#!/usr/bin/env python3
"""Check that _patch_network_util() still works against ESPHome's network code.

The halow component splices a USE_HALOW branch into ESPHome's
network::is_connected() and get_ip_addresses() at build time (see
docs/ESPHOME_COMPAT.md). This fetches network/util.h and util.cpp for each
given ESPHome release from GitHub, runs the real patch function on them and
reports where each hook landed. Run it before bumping the supported ESPHome
version, and after changing the patch code.

    .venv/bin/python esphome-halow/tools/test_network_patch.py
    .venv/bin/python esphome-halow/tools/test_network_patch.py 2026.9.1 2026.10.0

Needs an environment with ESPHome installed (any recent version; only the
component's Python is imported, not the version under test). Exits non-zero
if any release fails.
"""

import pathlib
import re
import sys
import tempfile
import types
import urllib.request

DEFAULT_VERSIONS = ["2025.6.0", "2025.12.0", "2026.4.0", "2026.9.1"]
RAW = "https://raw.githubusercontent.com/esphome/esphome/{ref}/esphome/components/network/{name}"
HOOK_RE = r"\b{fn}\(\) \{{\n#ifdef USE_HALOW\n"

repo = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(repo / "components"))

import esphome.components.network as network_module  # noqa: E402
from esphome.core import EsphomeError  # noqa: E402

import halow  # noqa: E402


def fetch(version, dest):
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("util.h", "util.cpp"):
        try:
            with urllib.request.urlopen(RAW.format(ref=version, name=name), timeout=30) as r:
                (dest / name).write_bytes(r.read())
        except OSError as err:
            raise SystemExit(f"{version}: could not fetch {name}: {err}") from err


def run_patch(src_dir):
    """Run halow._patch_network_util() against src_dir; return {file: text}."""
    out_dir = pathlib.Path(tempfile.mkdtemp())
    registered = {}
    network_module.__file__ = str(src_dir / "__init__.py")
    halow.CORE = types.SimpleNamespace(
        relative_build_path=lambda *parts: str(out_dir.joinpath(*parts))
    )
    halow.add_extra_build_file = lambda name, path: registered.__setitem__(name, path)
    halow._patch_network_util()
    return {name.rsplit("/", 1)[-1]: pathlib.Path(p).read_text() for name, p in registered.items()}


def main():
    versions = sys.argv[1:] or DEFAULT_VERSIONS
    work = pathlib.Path(tempfile.mkdtemp())
    failed = False

    for version in versions:
        src = work / version
        fetch(version, src)
        try:
            patched = run_patch(src)
        except EsphomeError as err:
            print(f"FAIL {version}: {err}")
            failed = True
            continue
        hooks = {
            fn: name
            for name, text in patched.items()
            for fn in ("is_connected", "get_ip_addresses")
            if re.search(HOOK_RE.format(fn=fn), text)
        }
        ok = set(hooks) == {"is_connected", "get_ip_addresses"}
        failed |= not ok
        print(f"{'ok  ' if ok else 'FAIL'} {version}: is_connected -> "
              f"{hooks.get('is_connected', 'missing')}, get_ip_addresses -> "
              f"{hooks.get('get_ip_addresses', 'missing')}")

    # A layout the patch can't handle must stop the build, not pass silently
    broken = work / "broken"
    broken.mkdir()
    for name in ("util.h", "util.cpp"):
        text = (work / versions[-1] / name).read_text()
        (broken / name).write_text(re.sub(r"\bis_connected\(\)", "is_up()", text))
    try:
        run_patch(broken)
        print("FAIL broken layout: patch did not raise")
        failed = True
    except EsphomeError:
        print("ok   broken layout: build stops with an error")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
