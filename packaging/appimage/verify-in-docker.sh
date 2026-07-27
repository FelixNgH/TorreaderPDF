#!/usr/bin/env bash
set -euo pipefail

DEB="${1:?Usage: $0 <path-to-deb> [docker-image]}"
IMAGE="${2:-ubuntu:24.04}"
DEB_DIR="$(cd "$(dirname "$DEB")" && pwd)"
DEB_NAME="$(basename "$DEB")"

exec docker run --rm -v "$DEB_DIR:/pkg" "$IMAGE" bash -c "
set -euo pipefail

echo '=== verify-in-docker: $IMAGE ==='

apt-get update -qq
apt-get install -y -qq xvfb x11-utils ./pkg/$DEB_NAME

Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb.log 2>&1 &
XVFB_PID=\$!
export DISPLAY=:99

for i in \$(seq 1 15); do
  if xdpyinfo >/dev/null 2>&1; then break; fi
  sleep 1
done

/usr/bin/torreader >/tmp/app.log 2>&1 &
PID=\$!

PASS=0
for i in \$(seq 1 20); do
  if ! kill -0 \$PID 2>/dev/null; then
    echo '=== app exited early, last 20 log lines ==='
    tail -20 /tmp/app.log
    echo 'VERIFY: FAIL app exited before showing window'
    kill \$XVFB_PID 2>/dev/null || true
    exit 1
  fi
  if xwininfo -root -tree 2>/dev/null | grep -qi 'TorReader'; then
    PASS=1
    break
  fi
  sleep 1
done

if [ \"\$PASS\" -eq 0 ]; then
  echo '=== timeout, last 20 log lines ==='
  tail -20 /tmp/app.log
  kill \$PID 2>/dev/null || true
  kill \$XVFB_PID 2>/dev/null || true
  echo 'VERIFY: FAIL no TorReader window within 20s'
  exit 1
fi

echo '  window check: PASS'

desktop-file-validate /usr/share/applications/cloud.torreader.TorReader.desktop
echo '  desktop-file-validate: PASS'

xdg-mime query default application/pdf || true

if command -v appstreamcli >/dev/null 2>&1; then
  appstreamcli validate /usr/share/metainfo/cloud.torreader.TorReader.metainfo.xml || true
fi

kill \$PID 2>/dev/null || true
kill \$XVFB_PID 2>/dev/null || true
echo 'VERIFY: PASS'
"
