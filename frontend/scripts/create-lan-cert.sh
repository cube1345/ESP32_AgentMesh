#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CERT_DIR="${ROOT_DIR}/certs"
KEY_PATH="${CERT_DIR}/espagent-lan.key"
CERT_PATH="${CERT_DIR}/espagent-lan.crt"
CONF_PATH="${CERT_DIR}/espagent-lan.openssl.cnf"

LAN_IP="${1:-}"
if [[ -z "${LAN_IP}" ]]; then
  LAN_IP="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<=NF; i++) if ($i=="src") {print $(i+1); exit}}')"
fi
if [[ -z "${LAN_IP}" ]]; then
  echo "Cannot detect LAN IP. Usage: $0 <LAN_IP>" >&2
  exit 1
fi

mkdir -p "${CERT_DIR}"

cat > "${CONF_PATH}" <<EOF
[req]
default_bits = 2048
prompt = no
default_md = sha256
distinguished_name = dn
x509_extensions = v3_req

[dn]
CN = espagent-lan

[v3_req]
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
IP.2 = ${LAN_IP}
EOF

openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
  -keyout "${KEY_PATH}" \
  -out "${CERT_PATH}" \
  -config "${CONF_PATH}"

chmod 600 "${KEY_PATH}"

echo "Created LAN HTTPS cert:"
echo "  key:  ${KEY_PATH}"
echo "  cert: ${CERT_PATH}"
echo
echo "Start frontend with:"
echo "  cd ${ROOT_DIR}"
echo "  npm run dev:https"
echo
echo "Open from LAN devices:"
echo "  https://${LAN_IP}:4173/"
