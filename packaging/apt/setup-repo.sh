#!/usr/bin/env bash
set -euo pipefail

generate_release() {
  local repo_root="$1" release_dir="$2"
  cat > "$repo_root/aptftp.conf" <<'CONF'
APT::FTPArchive::Release::Origin "TorReader";
APT::FTPArchive::Release::Label "TorReader";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Architectures "amd64";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Description "TorReader PDF official package repository";
CONF
  if command -v apt-ftparchive >/dev/null 2>&1; then
    apt-ftparchive -c "$repo_root/aptftp.conf" release "$release_dir" > "$release_dir/Release"
  elif command -v docker >/dev/null 2>&1; then
    touch "$release_dir/Release"
    docker run --rm -v "$repo_root:/repo" -w /repo/dists/stable ubuntu:24.04 \
      bash -c 'apt-get update -qq && apt-get install -y -qq apt-utils >/dev/null && \
               apt-ftparchive -c /repo/aptftp.conf release . > /tmp/Release && \
               cat /tmp/Release > /repo/dists/stable/Release'
  else
    echo "ERROR: neither apt-ftparchive nor docker found, cannot generate Release file" >&2
    exit 1
  fi
  rm -f "$repo_root/aptftp.conf"
}

DEB="${1:?Usage: $0 <path-to-deb> [output-dir]}"
OUT="${2:-/home/felix2/out/apt}"

[ -f "$DEB" ] || { echo "ERROR: deb not found: $DEB" >&2; exit 1; }

VER="$(dpkg-deb -f "$DEB" Version)"
ARCH="$(dpkg-deb -f "$DEB" Architecture)"
DEB_NAME="torreader_${VER}_${ARCH}.deb"

GNUPGHOME="${HOME}/.torreader-apt-gpg"
export GNUPGHOME
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"

KEY_EMAIL="felixnguyenhuy@gmail.com"

if gpg --list-keys "$KEY_EMAIL" >/dev/null 2>&1; then
  echo "Reusing existing signing key for $KEY_EMAIL"
else
  echo "Generating new signing key (no passphrase)..."
  gpg --batch --gen-key <<EOF
Key-Type: RSA
Key-Length: 4096
Name-Real: TorReader Repository Signing Key
Name-Email: $KEY_EMAIL
Expire-Date: 0
%no-protection
%commit
EOF
fi

FPR="$(gpg --with-colons --fingerprint "$KEY_EMAIL" | awk -F: '/^fpr:/ {print $10; exit}')"

mkdir -p "$OUT/pool/main/t/torreader"
cp "$DEB" "$OUT/pool/main/t/torreader/$DEB_NAME"

mkdir -p "$OUT/dists/stable/main/binary-amd64"

cd "$OUT"
dpkg-scanpackages --arch amd64 pool/ > "dists/stable/main/binary-amd64/Packages"
gzip -kf "dists/stable/main/binary-amd64/Packages"

cd "$OUT/dists/stable"
rm -f Release Release.gpg InRelease
generate_release "$OUT" .

gpg --clearsign -o InRelease Release
gpg -abs -o Release.gpg Release

cd "$OUT"
gpg --export > torreader-archive-keyring.gpg

echo
echo "=== APT repository built at: $OUT ==="
echo
echo "Signing key fingerprint: $FPR"
echo
echo "=== User install commands ==="
echo "sudo mkdir -p /etc/apt/keyrings"
echo "sudo curl -fsSL https://torreader.cloud/apt/torreader-archive-keyring.gpg -o /etc/apt/keyrings/torreader.gpg"
echo "echo \"deb [signed-by=/etc/apt/keyrings/torreader.gpg] https://torreader.cloud/apt stable main\" | sudo tee /etc/apt/sources.list.d/torreader.list > /dev/null"
echo "sudo apt update && sudo apt install torreader"
