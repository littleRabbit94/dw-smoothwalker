"""Build the SmoothCam - UE4SS (DWSmoothCam) archive for Nexus.

Output: dist/SmoothCam-UE4SS-<version>.zip. The archive carries the ue4ss/Mods/DWSmoothCam/ path, so
extracting it into Dawnwalker\Binaries\Win64 installs the mod:

    ue4ss/Mods/DWSmoothCam/dlls/main.dll
    ue4ss/Mods/DWSmoothCam/enabled.txt
    ue4ss/Mods/DWSmoothCam/LICENSE
    ue4ss/Mods/DWSmoothCam/mod_settings.ini
    ue4ss/Mods/DWSmoothCam/scripts/config/smoothcam.ini

No PDB, and no presets.ini: the mod creates it on the first slot save.

Source: ue4ss/DWSmoothCam/ in this repo; main.dll is the git-ignored build output of cpp/DWSmoothCam.
Version: ModVersion in dllmain.cpp. The build fails if:
  - ModVersion, mod_settings.ini [Mod] Version and the newest entry in the changelog block of
    nexus-page-metadata.md disagree;
  - a ConfigKey in mod_settings.ini is missing from smoothcam.ini or present more than once (either
    stops the Mod Menu page opening);
  - a shipped smoothcam.ini value is outside its setting's Minimum/Maximum or not in its PresetValues;
  - main.dll is older than the newest file in cpp/DWSmoothCam/src (rebuild first).

Usage: python Build-Package.py
"""
from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MOD = REPO / "ue4ss" / "DWSmoothCam"
SRC = REPO / "cpp" / "DWSmoothCam" / "src"
SHEET = HERE / "nexus-page-metadata.md"
DIST = HERE / "dist"
MOD_NAME = "DWSmoothCam"

BUILD_HINT = ("cmake --build build "
              "--config Game__Shipping__Win64 --target DWSmoothCam --parallel")


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
            errors.append(f"{key}: {len(got)} lines in smoothcam.ini, needs exactly 1")
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
        fail("mod_settings.ini vs smoothcam.ini:\n  " + "\n  ".join(errors))
    return count


def check_dll_fresh(dll: Path) -> None:
    if not dll.is_file():
        fail(f"{dll} missing; build it:\n  {BUILD_HINT}")
    newest = max((p for p in SRC.rglob("*") if p.is_file()), key=lambda p: p.stat().st_mtime)
    if dll.stat().st_mtime < newest.stat().st_mtime:
        fail(f"main.dll is older than src/{newest.name}; rebuild:\n  {BUILD_HINT}")


def main() -> int:
    manifest = MOD / "mod_settings.ini"
    ini = MOD / "scripts" / "config" / "smoothcam.ini"
    dll = MOD / "dlls" / "main.dll"
    for p in (manifest, ini, MOD / "LICENSE", MOD / "enabled.txt"):
        if not p.is_file():
            fail(f"missing {p}")

    ver = mod_version()
    sections = manifest_sections(manifest)
    check_versions(ver, sections)
    settings = check_settings(sections, ini_values(ini))
    check_dll_fresh(dll)

    DIST.mkdir(exist_ok=True)
    out = DIST / f"SmoothCam-UE4SS-{ver}.zip"
    prefix = f"ue4ss/Mods/{MOD_NAME}/"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, prefix + "dlls/main.dll")
        z.writestr(prefix + "enabled.txt", "")
        z.write(MOD / "LICENSE", prefix + "LICENSE")
        z.write(manifest, prefix + "mod_settings.ini")
        z.write(ini, prefix + "scripts/config/smoothcam.ini")
    print(f"version {ver}")
    print(f"{settings} Mod Menu settings checked against smoothcam.ini")
    print(out, f"{out.stat().st_size:,} bytes")
    for i in zipfile.ZipFile(out).infolist():
        print(f"  {i.file_size:>9,} {i.filename}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
