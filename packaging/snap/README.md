# Snap package for TorReader PDF

## Build locally (requires snapcraft)

```bash
snapcraft -p packaging/snap
sudo snap install --dangerous torreader_*.snap
torreader
```

## Build via CI

Push a tag `v*` or trigger `workflow_dispatch` on the Snap workflow in
GitHub Actions. The built snap is uploaded as an artifact.

## Publish to Snap Store

1. Register the name (one-time, needs Snap Store account):
   ```bash
   snapcraft register torreader
   ```

2. Export credentials from the store owner's machine:
   ```bash
   snapcraft export-login --snaps torreader --acls package_release,snap_debug_symbols -
   ```
   Copy the output into a GitHub secret `SNAPCRAFT_STORE_CREDENTIALS`.

3. Add a step to `.github/workflows/snap.yml`:
   ```yaml
   - name: Publish to Snap Store
     run: snapcraft upload --release=stable torreader_*.snap
     env:
       SNAPCRAFT_STORE_CREDENTIALS: ${{ secrets.SNAPCRAFT_STORE_CREDENTIALS }}
   ```

Steps 1–2 must be done by the Snap Store account owner.
Step 3 is a one-line uncomment in the existing workflow.
