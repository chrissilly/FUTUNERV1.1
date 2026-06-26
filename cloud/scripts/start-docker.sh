#!/usr/bin/env bash
# ============================================================================
# start-docker.sh
#
# Start (or restart) the FUTUNER cloud Docker container on the server.
#
# Runs on the CentOS server in /opt/srm-cloud/ — assumes:
#   - Docker daemon installed + running
#   - cloud source rsync'd to /opt/srm-cloud/ from your Mac
#   - Apache reverse-proxy already configured for /fut/
#
# Idempotent: if a container is already running, it's torn down cleanly
# before the new one starts. Persistent volumes (data/, firmware/,
# calibrations/) survive.
#
# Usage:
#   sudo /opt/srm-cloud/scripts/start-docker.sh
#
# Optional environment overrides:
#   DEPLOY_PATH=/opt/srm-cloud         (where the cloud bundle lives)
#   PUBLIC_URL=https://sillyrabbitmotorsport.com/fut
#   HEALTH_TIMEOUT_SEC=60               (how long to wait for /health to pass)
# ============================================================================

set -euo pipefail

# ─── CONFIG ─────────────────────────────────────────────────────────────────

DEPLOY_PATH="${DEPLOY_PATH:-/opt/srm-cloud}"
PUBLIC_URL="${PUBLIC_URL:-https://sillyrabbitmotorsport.com/fut}"
BACKEND_PORT="${BACKEND_PORT:-8000}"
COMPOSE_SERVICE="${COMPOSE_SERVICE:-srm-cloud}"
HEALTH_TIMEOUT_SEC="${HEALTH_TIMEOUT_SEC:-60}"

# ─── COLORS ─────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()  { echo -e "${BLUE}[$(date +'%H:%M:%S')]${NC} $*"; }
ok()   { echo -e "${GREEN}[✓]${NC} $*"; }
warn() { echo -e "${YELLOW}[!]${NC} $*"; }
err()  { echo -e "${RED}[✗]${NC} $*" >&2; }
die()  { err "$*"; exit 1; }

# ─── PREFLIGHT ──────────────────────────────────────────────────────────────

log "=== FUTUNER cloud Docker start ==="

[[ $EUID -eq 0 ]] || die "Run as root: sudo $0"

# Docker daemon
if ! systemctl is-active --quiet docker; then
  warn "Docker daemon not running — starting it"
  systemctl start docker
  sleep 2
fi
systemctl is-active --quiet docker || die "Docker failed to start"
ok "Docker daemon running"

# docker compose v2 plugin
if ! docker compose version &>/dev/null; then
  die "docker compose v2 plugin missing. Install: yum install docker-compose-plugin"
fi
ok "docker compose v2 available"

# Deploy path + required files
[[ -d "$DEPLOY_PATH" ]] || die "Deploy path missing: $DEPLOY_PATH (rsync first)"
cd "$DEPLOY_PATH"

for f in docker-compose.yml Dockerfile requirements.txt src/main.py; do
  [[ -e "$f" ]] || die "Missing required file: $DEPLOY_PATH/$f (incomplete rsync?)"
done
ok "Deploy path ready: $DEPLOY_PATH"

# Persistent volume dirs
for d in data firmware calibrations; do
  mkdir -p "$DEPLOY_PATH/$d"
done
ok "Persistent dirs: data/ firmware/ calibrations/"

# ─── STOP EXISTING (IDEMPOTENT) ─────────────────────────────────────────────

if docker compose ps --quiet 2>/dev/null | grep -q .; then
  log "Existing container(s) found — stopping cleanly"
  docker compose down
  ok "Old container(s) stopped"
fi

# ─── BUILD ──────────────────────────────────────────────────────────────────

log "Building container (this may take a few minutes on first run)..."
docker compose build --no-cache 2>&1 | tail -20
ok "Build complete"

# ─── START ──────────────────────────────────────────────────────────────────

log "Starting container detached..."
docker compose up -d
sleep 2

# Show what's running
docker compose ps

# ─── HEALTH CHECK (LOCAL) ───────────────────────────────────────────────────

log "Waiting up to ${HEALTH_TIMEOUT_SEC}s for /health on 127.0.0.1:${BACKEND_PORT}..."

deadline=$((SECONDS + HEALTH_TIMEOUT_SEC))
healthy=0
while (( SECONDS < deadline )); do
  if response="$(curl -fsS --max-time 3 "http://127.0.0.1:${BACKEND_PORT}/health" 2>/dev/null)"; then
    healthy=1
    break
  fi
  sleep 2
done

if (( healthy )); then
  ok "Local health PASS: $response"
else
  warn "Local /health did NOT respond in ${HEALTH_TIMEOUT_SEC}s"
  warn "Showing recent container logs:"
  docker compose logs --tail 30 "$COMPOSE_SERVICE"
  die "Container not healthy — investigate logs"
fi

# ─── HEALTH CHECK (PUBLIC, VIA APACHE) ──────────────────────────────────────

log "Verifying public URL via Apache: ${PUBLIC_URL}/health"

if response="$(curl -fsS --max-time 10 "${PUBLIC_URL}/health" 2>/dev/null)"; then
  ok "Public health PASS: $response"
else
  warn "Public /health failed — Apache may not be configured, or DNS not propagated"
  warn "  Check: curl -v ${PUBLIC_URL}/health"
  warn "  Apache vhost: grep -A20 '<Location /fut' /etc/httpd/conf.d/sites.conf"
  warn "  Apache logs:  tail -20 /var/log/httpd/*error*"
fi

# ─── SUMMARY ────────────────────────────────────────────────────────────────

echo
log "=== Done ==="
echo
echo "Container:    $(docker compose ps --format json | head -1 || echo 'unknown')"
echo "Local:        http://127.0.0.1:${BACKEND_PORT}/"
echo "Public:       ${PUBLIC_URL}/"
echo "Logs (tail):  docker compose -p $(basename $DEPLOY_PATH) logs -f ${COMPOSE_SERVICE}"
echo "Stop:         cd $DEPLOY_PATH && docker compose down"
echo "Restart:      $0"
echo
echo "Operational next steps (per upload2server.md):"
echo "  3. Enroll a dongle MAC + save auth_token"
echo "  4. Install auth_token on dongle via WS"
echo "  5. Mark device paid: 1"
echo "  6. (Optional) Upload SBF + assign to device"
echo "  7. (Optional) Upload firmware build for OTA"
echo "  8. Verify /admin/devices shows fully provisioned"
