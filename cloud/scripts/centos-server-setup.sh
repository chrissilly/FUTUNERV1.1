#!/usr/bin/env bash
# ============================================================================
# centos-server-setup.sh
#
# Comprehensive CentOS / RHEL / Rocky / AlmaLinux server bootstrap for the
# FUTUNER cloud (FastAPI in Docker, fronted by Apache reverse-proxy with
# Let's Encrypt SSL).
#
# What it does (in order):
#   1. Preflight: root check, OS detection, network reachability
#   2. System update + EPEL repo
#   3. Apache (httpd) + required modules
#   4. Docker CE + docker-compose v2 plugin
#   5. firewalld: open 80/443
#   6. SELinux: allow httpd to reverse-proxy to network (without disabling)
#   7. Certbot for Let's Encrypt
#   8. Deployment directory at /opt/srm-cloud + persistent volume dirs
#   9. Apache vhost (HTTP redirect + HTTPS reverse-proxy to 127.0.0.1:8000)
#  10. Acquire SSL cert via certbot (Apache plugin)
#  11. Enable services at boot (httpd, docker, certbot renewal timer)
#  12. Health checks
#
# Idempotent where reasonable. Re-runnable. Logs everything to a file.
#
# Usage:
#   1. scp this script to the server:
#        scp centos-server-setup.sh root@<server>:/root/
#   2. Edit the CONFIG block below to match your domain + email.
#   3. ssh root@<server> and run:
#        chmod +x /root/centos-server-setup.sh
#        /root/centos-server-setup.sh
#
# Requires: root, network access, DNS already pointing to this box.
# ============================================================================

set -euo pipefail

# ─── CONFIG ─────────────────────────────────────────────────────────────────
# REQUIRED — edit these before running.

DOMAIN="sillyrabbitmotorsport.com"
ADMIN_EMAIL="sean@sillyrabbitmotorsport.com"     # Let's Encrypt notifications
DEPLOY_PATH="/opt/srm-cloud"                      # where cloud code lives
BACKEND_PORT="8000"                               # FastAPI listens here

# Optional — defaults usually fine
LOG_FILE="/var/log/srm-cloud-setup.log"
SKIP_CERTBOT="${SKIP_CERTBOT:-0}"                 # set to 1 to skip cert step
                                                  # (useful if DNS not propagated yet)
MAX_UPLOAD_MB="50"                                # Apache LimitRequestBody
PROXY_TIMEOUT_SEC="300"                           # for long firmware uploads

# ─── COLORS & LOGGING ───────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

mkdir -p "$(dirname "$LOG_FILE")"
exec > >(tee -a "$LOG_FILE") 2>&1

log()  { echo -e "${BLUE}[$(date +'%Y-%m-%d %H:%M:%S')]${NC} $*"; }
ok()   { echo -e "${GREEN}[✓]${NC} $*"; }
warn() { echo -e "${YELLOW}[!]${NC} $*"; }
err()  { echo -e "${RED}[✗]${NC} $*" >&2; }
die()  { err "$*"; exit 1; }

# ─── PREFLIGHT ──────────────────────────────────────────────────────────────

log "=== FUTUNER cloud server bootstrap starting ==="
log "Logging to $LOG_FILE"

[[ $EUID -eq 0 ]] || die "Must run as root. Try: sudo $0"

if [[ -f /etc/os-release ]]; then
  . /etc/os-release
  OS_ID="${ID:-unknown}"
  OS_VER="${VERSION_ID:-unknown}"
  OS_MAJOR="${OS_VER%%.*}"
  log "OS: $OS_ID $OS_VER (major=$OS_MAJOR)"
else
  die "Cannot detect OS — /etc/os-release missing"
fi

case "$OS_ID" in
  centos|rocky|almalinux|rhel|ol)
    ok "Supported OS family"
    ;;
  *)
    warn "Untested OS family ($OS_ID) — proceeding anyway, may need tweaks"
    ;;
esac

# Pick package manager
if command -v dnf &>/dev/null; then
  PKG_MGR="dnf"
elif command -v yum &>/dev/null; then
  PKG_MGR="yum"
else
  die "No yum/dnf found — unsupported system"
fi
log "Package manager: $PKG_MGR"

# Network reachability
log "Checking network reachability..."
if curl -fsS --max-time 10 https://www.google.com &>/dev/null; then
  ok "Network OK"
else
  warn "Outbound HTTPS check failed — package install will likely fail"
fi

# DNS check (warn-only — domain may not resolve yet at first run)
log "Checking DNS for $DOMAIN..."
DOMAIN_IP="$(dig +short "$DOMAIN" | tail -1 || true)"
SERVER_IP="$(curl -fsS --max-time 5 https://api.ipify.org || echo unknown)"
if [[ -n "$DOMAIN_IP" && "$DOMAIN_IP" == "$SERVER_IP" ]]; then
  ok "DNS: $DOMAIN → $DOMAIN_IP (matches this box)"
elif [[ -n "$DOMAIN_IP" ]]; then
  warn "DNS: $DOMAIN → $DOMAIN_IP, but this box appears to be $SERVER_IP"
  warn "  Let's Encrypt cert acquisition will fail until DNS is correct."
  warn "  Set SKIP_CERTBOT=1 to defer the cert step."
else
  warn "DNS: $DOMAIN does not resolve — set the A record before running certbot"
fi

# ─── 1. SYSTEM UPDATE + EPEL ────────────────────────────────────────────────

log "=== Step 1: System update + EPEL ==="
$PKG_MGR -y update
$PKG_MGR -y install epel-release || warn "EPEL already present or unavailable"
$PKG_MGR -y install dnf-plugins-core 2>/dev/null || \
  $PKG_MGR -y install yum-utils
ok "System updated"

# ─── 2. APACHE (httpd) + MODULES ────────────────────────────────────────────

log "=== Step 2: Apache + modules ==="
$PKG_MGR -y install httpd mod_ssl

# Confirm needed modules are present (they ship with httpd on RHEL family)
for mod in proxy proxy_http ssl headers rewrite; do
  if httpd -M 2>/dev/null | grep -qi "${mod}_module"; then
    ok "Apache module loaded: $mod"
  else
    warn "Apache module NOT loaded: $mod — check /etc/httpd/conf.modules.d/"
  fi
done

systemctl enable httpd
ok "Apache enabled at boot"

# ─── 3. DOCKER CE + COMPOSE V2 ──────────────────────────────────────────────

log "=== Step 3: Docker + docker-compose ==="

# Add Docker CE repo (works for CentOS 7/8, Rocky/Alma 8/9, RHEL 8/9)
if [[ ! -f /etc/yum.repos.d/docker-ce.repo ]]; then
  $PKG_MGR config-manager --add-repo \
    https://download.docker.com/linux/centos/docker-ce.repo
  ok "Docker CE repo added"
else
  ok "Docker CE repo already present"
fi

$PKG_MGR -y install docker-ce docker-ce-cli containerd.io docker-compose-plugin

systemctl enable --now docker
ok "Docker enabled and started"

docker --version
docker compose version

# ─── 4. FIREWALLD: OPEN 80/443 ──────────────────────────────────────────────

log "=== Step 4: firewalld ==="

if systemctl is-active --quiet firewalld; then
  firewall-cmd --permanent --add-service=http   || true
  firewall-cmd --permanent --add-service=https  || true
  firewall-cmd --reload
  ok "firewalld: 80/443 open"
else
  warn "firewalld not active — assuming network filtering is handled elsewhere"
fi

# ─── 5. SELINUX: ALLOW HTTPD REVERSE-PROXY ──────────────────────────────────

log "=== Step 5: SELinux ==="

SE_STATUS="$(getenforce 2>/dev/null || echo Disabled)"
log "SELinux status: $SE_STATUS"

if [[ "$SE_STATUS" == "Enforcing" || "$SE_STATUS" == "Permissive" ]]; then
  # Without these, httpd's ProxyPass to 127.0.0.1:8000 fails with
  # "Permission denied" from AVC denials.
  setsebool -P httpd_can_network_connect 1
  setsebool -P httpd_can_network_relay 1
  ok "SELinux: httpd_can_network_connect + relay enabled"
else
  log "SELinux disabled — no changes needed"
fi

# ─── 6. CERTBOT ─────────────────────────────────────────────────────────────

log "=== Step 6: Certbot ==="

$PKG_MGR -y install certbot python3-certbot-apache
ok "Certbot installed"

# ─── 7. DEPLOYMENT DIRECTORY ────────────────────────────────────────────────

log "=== Step 7: Deployment directory ==="

mkdir -p "$DEPLOY_PATH"
mkdir -p "$DEPLOY_PATH/data" "$DEPLOY_PATH/firmware" "$DEPLOY_PATH/calibrations"
chown -R root:root "$DEPLOY_PATH"
chmod 755 "$DEPLOY_PATH"
ok "Deployment directory: $DEPLOY_PATH (with data/, firmware/, calibrations/ subdirs)"

# Optional: persistent log dir for Docker
mkdir -p /var/log/srm-cloud
chmod 755 /var/log/srm-cloud
ok "Log directory: /var/log/srm-cloud"

# ─── 8. APACHE VHOST ────────────────────────────────────────────────────────

log "=== Step 8: Apache vhost ==="

# We write a "bootstrap" vhost (HTTP-only) first. Certbot will then upgrade
# it to include HTTPS in step 9 via --apache plugin. Re-running idempotently
# is safe; certbot detects existing config.

VHOST_CONF="/etc/httpd/conf.d/srm-cloud.conf"

cat > "$VHOST_CONF" <<EOF
# srm-cloud.conf — generated by centos-server-setup.sh
# Reverse-proxies $DOMAIN to local Docker container on port $BACKEND_PORT.
#
# After certbot runs, a second vhost on :443 will be added by certbot's
# Apache plugin. The :80 vhost will be modified to redirect HTTP → HTTPS.

<VirtualHost *:80>
    ServerName $DOMAIN

    # Let's Encrypt ACME challenge — must remain reachable on :80
    Alias /.well-known/acme-challenge/ /var/www/html/.well-known/acme-challenge/
    <Directory "/var/www/html/.well-known/acme-challenge/">
        Options None
        AllowOverride None
        Require all granted
    </Directory>

    ProxyPreserveHost On
    ProxyRequests Off

    # Avoid proxying ACME challenge paths
    ProxyPass /.well-known/acme-challenge/ !
    ProxyPass /        http://127.0.0.1:$BACKEND_PORT/
    ProxyPassReverse / http://127.0.0.1:$BACKEND_PORT/

    # Upload limits for firmware/SBF
    LimitRequestBody $(( MAX_UPLOAD_MB * 1024 * 1024 ))

    # Proxy timeouts for long uploads
    ProxyTimeout $PROXY_TIMEOUT_SEC
    Timeout      $PROXY_TIMEOUT_SEC

    ErrorLog  /var/log/httpd/srm-cloud-error.log
    CustomLog /var/log/httpd/srm-cloud-access.log combined
</VirtualHost>
EOF

ok "Vhost written: $VHOST_CONF"

# Validate Apache config before starting
if httpd -t; then
  ok "Apache config syntax OK"
else
  die "Apache config invalid — see error above"
fi

systemctl restart httpd
sleep 2
if systemctl is-active --quiet httpd; then
  ok "Apache running"
else
  die "Apache failed to start — journalctl -u httpd"
fi

# ─── 9. SSL CERT VIA CERTBOT ────────────────────────────────────────────────

if [[ "$SKIP_CERTBOT" == "1" ]]; then
  warn "SKIP_CERTBOT=1 set — skipping SSL acquisition"
  warn "  Run later: certbot --apache -d $DOMAIN --non-interactive --agree-tos -m $ADMIN_EMAIL"
else
  log "=== Step 9: Acquire SSL cert ==="

  # Certbot --apache plugin: provisions cert AND modifies vhost to add :443
  # and HTTP→HTTPS redirect on :80
  if certbot --apache \
       -d "$DOMAIN" \
       --non-interactive \
       --agree-tos \
       -m "$ADMIN_EMAIL" \
       --redirect; then
    ok "SSL cert acquired and Apache reconfigured"
  else
    warn "Certbot failed — check DNS, then run manually:"
    warn "  certbot --apache -d $DOMAIN -m $ADMIN_EMAIL --agree-tos --redirect"
  fi

  # Verify renewal timer is enabled (installed with certbot package)
  if systemctl list-unit-files | grep -q certbot-renew.timer; then
    systemctl enable --now certbot-renew.timer
    ok "Certbot renewal timer enabled"
  elif systemctl list-unit-files | grep -q certbot.timer; then
    systemctl enable --now certbot.timer
    ok "Certbot renewal timer enabled"
  else
    warn "No certbot renewal timer found — set up cron manually:"
    warn "  echo '0 3 * * * certbot renew --quiet' | crontab -"
  fi
fi

# ─── 10. SUMMARY + HEALTH CHECKS ────────────────────────────────────────────

log "=== Step 10: Final health checks ==="

echo
echo "─── Service status ───"
for svc in httpd docker firewalld; do
  if systemctl is-active --quiet "$svc"; then
    ok "$svc: running"
  else
    warn "$svc: NOT running"
  fi
done

echo
echo "─── Firewall ───"
firewall-cmd --list-services 2>/dev/null || warn "firewalld not queryable"

echo
echo "─── Apache vhosts ───"
httpd -S 2>&1 | grep -E "namevhost|alias" || warn "no vhosts visible"

echo
echo "─── Docker ───"
docker info 2>&1 | head -5 || warn "docker info failed"

echo
echo "─── Cert ───"
if [[ -d "/etc/letsencrypt/live/$DOMAIN" ]]; then
  ok "Cert exists: /etc/letsencrypt/live/$DOMAIN/"
  certbot certificates 2>&1 | grep -A3 "$DOMAIN" || true
else
  warn "No cert at /etc/letsencrypt/live/$DOMAIN/ (skipped or failed)"
fi

echo
echo "─── HTTP/HTTPS reachability ───"
if curl -fsS --max-time 10 "http://$DOMAIN/" -o /dev/null 2>&1; then
  ok "HTTP reachable (will 502/upstream-not-ready until container is up)"
else
  warn "HTTP NOT reachable — check firewall / DNS / Apache logs"
fi

if [[ -d "/etc/letsencrypt/live/$DOMAIN" ]]; then
  if curl -fsS --max-time 10 "https://$DOMAIN/" -o /dev/null 2>&1; then
    ok "HTTPS reachable"
  else
    warn "HTTPS NOT reachable — check Apache SSL vhost config"
  fi
fi

echo
log "=== Bootstrap complete ==="
echo
echo "Next steps:"
echo "  1. rsync cloud source from your Mac:"
echo "       (on Mac) cd ~/esp/obd/FUTV1.1/cloud"
echo "                rsync -av --exclude='__pycache__' --exclude='.pytest_cache' \\"
echo "                  ./ root@$DOMAIN:$DEPLOY_PATH/"
echo
echo "  2. Build and start the container:"
echo "       cd $DEPLOY_PATH"
echo "       docker compose up -d --build"
echo "       docker compose logs -f srm-cloud"
echo
echo "  3. Verify:"
echo "       curl -fsS https://$DOMAIN/health"
echo "       # expect: {\"ok\":true,...}"
echo
echo "  4. Continue per upload2server.md §3 (enroll dongle, mark paid, etc.)"
echo
echo "Log file: $LOG_FILE"
