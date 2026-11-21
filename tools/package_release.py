"""Builds the Nexus archives for one version: the plugin with its INI and license notices, and the Korean notices.

Usage: python -I tools/package_release.py 1.0.0
Inputs: the release build (D:/DEV/build/SCSSoulCharge/release), dist/, dist-optional/Korean/, LICENSE, the vcpkg
copyright files of the linked packages. Output: release-<version>/ next to this repository's root.
"""

import argparse
import logging
import pathlib
import zipfile

log = logging.getLogger("package_release")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(r"D:\DEV\build\SCSSoulCharge\release")
VCPKG_SHARE = BUILD / "vcpkg_installed" / "x64-windows-static-md" / "share"
LINKED_PACKAGES = ("detours", "fmt", "spdlog", "directxmath", "directxtk", "nlohmann-json", "rapidcsv", "xbyak")
SOURCE_URL = "https://github.com/Iwillbewarlock/SCSSoulCharge/tree/standalone"


def third_party_notices() -> str:
    parts = [
        "SCSSoulCharge third-party notices\n",
        f"Source code (GPL-3.0): {SOURCE_URL}\n",
        "\n== CommonLibSSE-NG ==\nGPL-3.0-or-later WITH the Modding Exception AND the GPL-3.0 Linking Exception (with Corresponding Source).\n"
        "https://github.com/alandtse/CommonLibVR\n",
    ]
    for package in LINKED_PACKAGES:
        copyright_file = VCPKG_SHARE / package / "copyright"
        if not copyright_file.is_file():
            raise FileNotFoundError(f"{copyright_file} is missing: build the release first (build.cmd) so vcpkg installs {package}")
        parts.append(f"\n== {package} ==\n{copyright_file.read_text(encoding='utf-8', errors='replace').strip()}\n")
    return "".join(parts)


def write_zip(path: pathlib.Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    log.info("%s: %s", path.name, ", ".join(files))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    args = parser.parse_args()
    out = ROOT / f"release-{args.version}"
    out.mkdir(exist_ok=True)
    ini = (ROOT / "dist/SKSE/Plugins/SCSSoulCharge.ini").read_bytes()
    ini_text = ini.decode("utf-16")  # shipped as UTF-16 LE with BOM, so non-ASCII markers survive an edit
    if "bEnabled=1" in ini_text or "bRestoreNames=1" in ini_text:
        raise ValueError("dist INI must ship with bEnabled=0 and bRestoreNames=0")
    write_zip(out / f"SCSSoulCharge-{args.version}.zip", {
        "SKSE/Plugins/SCSSoulCharge.dll": (BUILD / "SCSSoulCharge.dll").read_bytes(),
        "SKSE/Plugins/SCSSoulCharge.ini": ini,
        "SKSE/Plugins/SCSSoulCharge-LICENSE.txt": (ROOT / "LICENSE").read_bytes(),
        "SKSE/Plugins/SCSSoulCharge-THIRD-PARTY-NOTICES.txt": third_party_notices().encode("utf-8"),
    })
    write_zip(out / f"SCSSoulCharge-Korean-Notices-{args.version}.zip", {
        "SKSE/Plugins/SCSSoulCharge_Messages.ini": (ROOT / "dist-optional/Korean/SKSE/Plugins/SCSSoulCharge_Messages.ini").read_bytes(),
    })


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    main()
