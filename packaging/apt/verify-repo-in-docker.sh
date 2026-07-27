#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-https://torreader.cloud/apt}"

exec docker run --rm ubuntu:24.04 bash -c "
set -euo pipefail

echo '=== verify-apt-repo: $BASE_URL ==='

apt-get update -qq
apt-get install -y -qq ca-certificates curl gnupg

mkdir -p /etc/apt/keyrings
curl -fsSL '$BASE_URL/torreader-archive-keyring.gpg' -o /etc/apt/keyrings/torreader.gpg

echo 'deb [signed-by=/etc/apt/keyrings/torreader.gpg] $BASE_URL stable main' > /etc/apt/sources.list.d/torreader.list

UPDATE_OUTPUT=\$(apt-get update 2>&1)
echo \"\$UPDATE_OUTPUT\"

if echo \"\$UPDATE_OUTPUT\" | grep -qiE 'NO_PUBKEY|not signed|GPG error|public key is not available'; then
  echo 'VERIFY: FAIL GPG key not accepted during apt update'
  exit 1
fi

apt-get install -y torreader 2>&1

dpkg -s torreader | head

echo 'VERIFY: PASS'
"
