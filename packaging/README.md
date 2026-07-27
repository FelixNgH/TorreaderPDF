# TorReader PDF — Linux Packaging

## Distribution channels

| Channel | Directory | Key file |
|---|---|---|
| **Flathub / AppStream** | `packaging/linux/` | `.metainfo.xml` + `.desktop` |
| **Snap Store** | n/a (extra work) | uses same `.desktop` |
| **AppImageHub** | `packaging/linux/` | `.metainfo.xml` + `.desktop` |
| **Arch Linux (AUR)** | `packaging/aur/` | `PKGBUILD` |
| **Debian / Ubuntu** | `packaging/deb/` | `build-deb.sh` |
| **AppImage repack** | `packaging/appimage/` | `repack-metadata.sh` |

## Release pipeline (3 steps, mandatory order)

```bash
# Step 1 — Repack AppImage with correct metadata
bash packaging/appimage/repack-metadata.sh TorreaderPDF-2.2.4-x86_64.AppImage TorreaderPDF-2.2.4-x86_64-repacked.AppImage

# Step 2 — Build .deb from repacked AppImage
bash packaging/deb/build-deb.sh TorreaderPDF-2.2.4-x86_64-repacked.AppImage [version]

# Step 3 — Verify GUI in clean Docker container (FAIL = do not release)
bash packaging/appimage/verify-in-docker.sh torreader_2.2.4_amd64.deb [optional-docker-image]
```

**Do not release unless verify-in-docker.sh exits 0.**

## Safety rule

Scripts in `packaging/` **must never** modify, strip, `patchelf`, or overwrite any
binary in `usr/bin/` and `usr/lib/`. The `repack-metadata.sh` script verifies
sha256 checksums before and after to enforce this.

# Build & verify commands

```bash
# Desktop entry validation
desktop-file-validate packaging/linux/cloud.torreader.TorReader.desktop

# AppStream metainfo validation
appstreamcli validate --pedantic packaging/linux/cloud.torreader.TorReader.metainfo.xml

# PKGBUILD syntax check
bash -n packaging/aur/PKGBUILD

# .deb builder syntax check
bash -n packaging/deb/build-deb.sh

# repack script syntax check
bash -n packaging/appimage/repack-metadata.sh

# verify-in-docker syntax check
bash -n packaging/appimage/verify-in-docker.sh

# Build .deb (requires a repacked AppImage)
bash packaging/deb/build-deb.sh path/to/TorreaderPDF-2.2.4-x86_64-repacked.AppImage [version]

# Repack AppImage metadata
bash packaging/appimage/repack-metadata.sh input.AppImage output.AppImage

# GUI verification in Docker
bash packaging/appimage/verify-in-docker.sh path/to/package.deb
```

## Bump version checklist

Update the version number **everywhere** listed below:

1. `packaging/linux/cloud.torreader.TorReader.desktop` — `X-AppImage-Version`
2. `packaging/linux/cloud.torreader.TorReader.metainfo.xml` — add new `<release>` entry
3. `packaging/aur/PKGBUILD` — `pkgver`
4. `packaging/deb/build-deb.sh` — default `VERSION` in script body

Also update the download URL in `PKGBUILD` and `build-deb.sh` if the release tag scheme changes.
