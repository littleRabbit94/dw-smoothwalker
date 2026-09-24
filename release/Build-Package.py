"""Build the Smoothwalker - Third Person Camera (DWSmoothwalker) archive for Nexus.

Output: release/dist/Smoothwalker-<version>.zip. The archive carries the ue4ss/Mods/DWSmoothwalker/
path, so extracting it into Dawnwalker\Binaries\Win64 installs the mod:

    ue4ss/Mods/DWSmoothwalker/dlls/main.dll
    ue4ss/Mods/DWSmoothwalker/enabled.txt
    ue4ss/Mods/DWSmoothwalker/LICENSE
    ue4ss/Mods/DWSmoothwalker/mod_settings.ini
    ue4ss/Mods/DWSmoothwalker/config/smoothwalker.ini

No PDB and no presets folder: the mod creates config/presets/ at startup.

Optional file: release/dist/Smoothwalker-Example-Preset-<version>.zip, the commented example in
release/example-preset/ at ue4ss/Mods/DWSmoothwalker/config/presets/<file name>.

Optional file: release/dist/Smoothwalker-Example-API-Mod-<version>.zip, the drop-in Lua mod in
release/example-consumer/ at ue4ss/Mods/<folder>/ (Scripts/main.lua, enabled.txt, LICENSE). luac -p
gates its syntax; the build fails when luac is not found.

Source: mod/ in this repo; mod/dlls/main.dll is the git-ignored build output of src/.
Version: ModVersion in src/dllmain.cpp. The build fails if:
  - ModVersion, mod_settings.ini [Mod] Version and the newest entry in the changelog block of
    nexus-page-metadata.md disagree;
  - a ConfigKey in mod_settings.ini is missing from smoothwalker.ini or present more than once (either
    stops the Mod Menu page opening);
  - a shipped smoothwalker.ini value is outside its setting's Minimum/Maximum or not in its PresetValues;
  - main.dll is older than the newest file in src/ (rebuild first);
  - mod/ has a scripts/ subfolder (UE4SS would log a red main.lua error on every start);
  - the example preset lacks a name line or a key from PRESET_KEYS (config.hpp), holds an unknown or
    repeated key, or has a value outside its Mod Menu range, off its Step, or not in PresetValues.

Usage: python release/Build-Package.py (from any directory)
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
MOD = REPO / "mod"
SRC = REPO / "src"
SHEET = HERE / "nexus-page-metadata.md"
EXAMPLE_DIR = HERE / "example-preset"
CONSUMER_DIR = HERE / "example-consumer"
LUAC_FALLBACK = Path.home() / "AppData/Local/Programs/Lua/bin/luac.exe"
DIST = HERE / "dist"
MOD_NAME = "DWSmoothwalker"

BUILD_HINT = ("cmake --build build "
              "--config Game__Shipping__Win64 --target DWSmoothwalker --parallel")


def fail(msg: str) -> None:
    sys.exit(f"FAIL: {msg}")


def mod_version() -> str:
    m = re.search(r'ModVersion\s*=\s*STR\("([^"]+)"\)', (SRC / "dllmain.cpp").read_text(encoding="utf-8"))
    if not m:
        fail("ModVersion not found in dllmain.cpp")
    return m.group(1)


def manifest_sections(manifest: Path) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = None
    for line in manifest.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith(";"):
            continue
        m = re.match(r"^\[(.+)\]$", s)
        if m:
            current = sections.setdefault(m.group(1), {})
            continue
        m = re.match(r"^([\w.]+)\s*=\s*(.*)$", s)
        if m and current is not None:
            current[m.group(1)] = m.group(2).strip()
    return sections


def ini_values(ini: Path) -> dict[str, list[str]]:
    """Every key = value line, comments stripped. A list per key, so duplicates show."""
    found: dict[str, list[str]] = {}
    for line in ini.read_text(encoding="utf-8").splitlines():
        s = line.split(";", 1)[0].strip()
        m = re.match(r"^([\w.]+)\s*=\s*(.*)$", s)
        if m:
            found.setdefault(m.group(1), []).append(m.group(2).strip())
    return found


def newest_changelog_version() -> str:
    text = SHEET.read_text(encoding="utf-8")
    head = text.find("## Changelog tab")
    if head < 0:
        fail(f"no '## Changelog tab' section in {SHEET.name}")
    m = re.search(r"^```\s*\n\s*(\d+(?:\.\d+)+)\s+-", text[head:], re.M)
    if not m:
        fail(f"no changelog entry found in {SHEET.name}")
    return m.group(1)


def check_versions(ver: str, sections: dict[str, dict[str, str]]) -> None:
    manifest_ver = sections.get("Mod", {}).get("Version")
    log_ver = newest_changelog_version()
    if not (ver == manifest_ver == log_ver):
        fail(f"versions disagree: dllmain.cpp ModVersion {ver!r}, mod_settings.ini [Mod] Version "
             f"{manifest_ver!r}, newest changelog entry {log_ver!r}")


def check_settings(sections: dict[str, dict[str, str]], values: dict[str, list[str]]) -> int:
    errors = []
    count = 0
    for name, sec in sections.items():
        if not name.startswith("Setting."):
            continue
        key = sec.get("ConfigKey")
        if not key:
            errors.append(f"[{name}] has no ConfigKey")
            continue
        count += 1
        got = values.get(key, [])
        if len(got) != 1:
            errors.append(f"{key}: {len(got)} lines in smoothwalker.ini, needs exactly 1")
            continue
        try:
            v = float(got[0])
        except ValueError:
            errors.append(f"{key} = {got[0]!r} is not a number")
            continue
        if "Minimum" in sec and v < float(sec["Minimum"]):
            errors.append(f"{key} = {got[0]} below Minimum {sec['Minimum']}")
        if "Maximum" in sec and v > float(sec["Maximum"]):
            errors.append(f"{key} = {got[0]} above Maximum {sec['Maximum']}")
        if "PresetValues" in sec and v not in [float(p) for p in sec["PresetValues"].split("|")]:
            errors.append(f"{key} = {got[0]} not in PresetValues {sec['PresetValues']}")
    if errors:
        fail("mod_settings.ini vs smoothwalker.ini:\n  " + "\n  ".join(errors))
    return count


def preset_keys() -> list[str]:
    text = (SRC / "config.hpp").read_text(encoding="utf-8")
    m = re.search(r"PRESET_KEYS\{(.*?)\};", text, re.S)
    if not m:
        fail("PRESET_KEYS not found in config.hpp")
    return re.findall(r'"(\w+)"', m.group(1))


def check_example(path: Path, sections: dict[str, dict[str, str]]) -> None:
    """The mod would clamp a bad value silently; the example is a template, so it must be exact."""
    if re.fullmatch(r"(?i)(slot \d+|mod_settings)\.ini", path.name):
        fail(f"example file name {path.name!r} collides with a save slot or the manifest")
    values = ini_values(path)
    names = values.pop("name", [])
    if len(names) != 1 or not names[0]:
        fail(f"{path.name}: needs exactly one non-empty name line")
    if len(names[0].encode("utf-8")) > 48 or re.search(r"[|;#\x00-\x1f\x7f]", names[0]):
        fail(f"{path.name}: name {names[0]!r} is over 48 bytes or holds | ; # or a control character")
    by_key = {s["ConfigKey"]: s for s in sections.values() if "ConfigKey" in s}
    keys = preset_keys()
    errors = [f"{k} missing" for k in keys if k not in values]
    for key, got in values.items():
        if key not in keys:
            errors.append(f"{key} is not a preset key")
            continue
        if len(got) != 1:
            errors.append(f"{key} appears {len(got)} times")
            continue
        sec = by_key[key]
        v = float(got[0])
        if "PresetValues" in sec:
            if v not in [float(x) for x in sec["PresetValues"].split("|")]:
                errors.append(f"{key} = {got[0]} not in {sec['PresetValues']}")
            continue
        lo, hi, step = float(sec["Minimum"]), float(sec["Maximum"]), float(sec.get("Step", "1"))
        steps = (v - lo) / step
        if not lo <= v <= hi or abs(steps - round(steps)) > 1e-6:
            errors.append(f"{key} = {got[0]} outside {lo:g}..{hi:g} or off step {step:g}")
    if errors:
        fail(f"{path.name}:\n  " + "\n  ".join(errors))


def consumer_mod() -> Path:
    """The one mod folder under example-consumer/, with its script syntax-checked by luac."""
    folders = [p for p in CONSUMER_DIR.iterdir() if p.is_dir()]
    if len(folders) != 1:
        fail(f"expected one mod folder in {CONSUMER_DIR}, found {len(folders)}")
    mod = folders[0]
    script = mod / "Scripts" / "main.lua"
    for p in (script, mod / "enabled.txt"):
        if not p.is_file():
            fail(f"missing {p}")
    luac = shutil.which("luac") or (str(LUAC_FALLBACK) if LUAC_FALLBACK.is_file() else None)
    if not luac:
        fail("luac not found on PATH; the example mod's syntax gate needs Lua 5.4")
    run = subprocess.run([luac, "-p", str(script)], capture_output=True, text=True)
    if run.returncode != 0:
        fail(f"luac -p {script.name}:\n  {run.stderr.strip()}")
    return mod


def check_dll_fresh(dll: Path) -> None:
    if not dll.is_file():
        fail(f"{dll} missing; build it:\n  {BUILD_HINT}")
    newest = max((p for p in SRC.rglob("*") if p.is_file()), key=lambda p: p.stat().st_mtime)
    if dll.stat().st_mtime < newest.stat().st_mtime:
        fail(f"main.dll is older than src/{newest.name}; rebuild:\n  {BUILD_HINT}")


def main() -> int:
    manifest = MOD / "mod_settings.ini"
    ini = MOD / "config" / "smoothwalker.ini"
    dll = MOD / "dlls" / "main.dll"
    for p in (manifest, ini, MOD / "LICENSE", MOD / "enabled.txt"):
        if not p.is_file():
            fail(f"missing {p}")

    ver = mod_version()
    sections = manifest_sections(manifest)
    check_versions(ver, sections)
    settings = check_settings(sections, ini_values(ini))
    check_dll_fresh(dll)
    if (MOD / "scripts").exists():
        fail("mod/scripts exists: UE4SS would start a Lua mod and log a red main.lua error")
    examples = sorted(EXAMPLE_DIR.glob("*.ini"))
    if len(examples) != 1:
        fail(f"expected one example preset in {EXAMPLE_DIR}, found {len(examples)}")
    check_example(examples[0], sections)
    consumer = consumer_mod()

    DIST.mkdir(exist_ok=True)
    out = DIST / f"Smoothwalker-{ver}.zip"
    prefix = f"ue4ss/Mods/{MOD_NAME}/"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, prefix + "dlls/main.dll")
        z.writestr(prefix + "enabled.txt", "")
        z.write(MOD / "LICENSE", prefix + "LICENSE")
        z.write(manifest, prefix + "mod_settings.ini")
        z.write(ini, prefix + "config/smoothwalker.ini")
    print(f"version {ver}")
    print(f"{settings} Mod Menu settings checked against smoothwalker.ini")
    print(out, f"{out.stat().st_size:,} bytes")
    for i in zipfile.ZipFile(out).infolist():
        print(f"  {i.file_size:>9,} {i.filename}")

    example_zip = DIST / f"Smoothwalker-Example-Preset-{ver}.zip"
    with zipfile.ZipFile(example_zip, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(examples[0], prefix + "config/presets/" + examples[0].name)
    print(example_zip, f"{example_zip.stat().st_size:,} bytes")
    for i in zipfile.ZipFile(example_zip).infolist():
        print(f"  {i.file_size:>9,} {i.filename}")

    consumer_zip = DIST / f"Smoothwalker-Example-API-Mod-{ver}.zip"
    cprefix = f"ue4ss/Mods/{consumer.name}/"
    with zipfile.ZipFile(consumer_zip, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(consumer / "Scripts" / "main.lua", cprefix + "Scripts/main.lua")
        z.writestr(cprefix + "enabled.txt", "")
        z.write(MOD / "LICENSE", cprefix + "LICENSE")
    print(consumer_zip, f"{consumer_zip.stat().st_size:,} bytes")
    for i in zipfile.ZipFile(consumer_zip).infolist():
        print(f"  {i.file_size:>9,} {i.filename}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
