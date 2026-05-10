# Claude Code — Rsync Cloud Source to Remote Server

> Pushes the latest `cloud/` source from your Mac to
> `futuner@api.sillyrabbitmotorsport.com`, rebuilds the Docker
> container, restarts it, verifies `/health`. No interactive
> questions. Fails fast if env vars aren't set.
>
> **Set env vars first** (one-time per shell). Password lives ONLY in
> your shell, never written to disk:
>
> ```
> export CLOUD_SSH_PASS='your-server-password'
> ```
>
> (Hostname and username are hardcoded:
> `futuner@api.sillyrabbitmotorsport.com`. Default deployment path is
> `/home/futuner/srm-cloud`. Override with `export CLOUD_SSH_PATH=...` if your
> server uses a different path.)
>
> Optional:
>
> ```
> export ADMIN_API_KEY='your-admin-api-key'   # only if you have it locally
> ```

---

## One-time setup if you don't have sshpass

```
brew install hudochenkov/sshpass/sshpass
```

If that tap fails:

```
brew install esolitos/ipa/sshpass
```

---

## Paste this into Claude Code

```
Rsync the cloud source to my remote server, rebuild + restart the
Docker container, verify /health. No interactive pauses. Don't write
code, don't commit.

Hardcoded server identity:
  HOST = api.sillyrabbitmotorsport.com
  USER = futuner
  PATH = ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud}   (env override allowed)

Inputs from shell (read at start; do not ask me):
- CLOUD_SSH_PASS  (REQUIRED) — server SSH password; passed to sshpass
- ADMIN_API_KEY   (optional) — admin probe in Phase 5 only

If CLOUD_SSH_PASS is unset → STOP, print "export CLOUD_SSH_PASS and
re-run", do nothing else.

If sshpass binary isn't on PATH → STOP, print "install sshpass:
brew install hudochenkov/sshpass/sshpass" and stop.

Read first:
- ~/esp/obd/FUTV1.1/CLAUDE.md
- ~/esp/obd/FUTV1.1/cloud/Dockerfile
- ~/esp/obd/FUTV1.1/cloud/docker-compose.yml
- ~/esp/obd/FUTV1.1/cloud/requirements.txt

==========================================================
PHASE 1 — Pre-flight (silent; PASS/FAIL line per check)
==========================================================

  which sshpass                                          (must exist)
  which rsync                                            (must exist)
  echo $CLOUD_SSH_PASS | wc -c                           (>1)
  cd ~/esp/obd/FUTV1.1/cloud && ls Dockerfile \
      docker-compose.yml src/main.py requirements.txt    (all 4 exist)

  Connectivity test:
    sshpass -e ssh -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout=5 \
        futuner@api.sillyrabbitmotorsport.com 'echo ok'
  (export SSHPASS=$CLOUD_SSH_PASS for sshpass -e to read it)
  Must print "ok".

If any pre-flight fails, STOP and print which.

==========================================================
PHASE 2 — Rsync source (excludes volumes + caches)
==========================================================

Set SSHPASS=$CLOUD_SSH_PASS in env for the duration.

  sshpass -e rsync -av \
      --exclude='__pycache__' \
      --exclude='.pytest_cache' \
      --exclude='data/' \
      --exclude='firmware/' \
      --exclude='calibrations/' \
      --exclude='*.pyc' \
      -e 'ssh -o StrictHostKeyChecking=accept-new' \
      ~/esp/obd/FUTV1.1/cloud/ \
      futuner@api.sillyrabbitmotorsport.com:${CLOUD_SSH_PATH:-/home/futuner/srm-cloud}/

Capture rsync stats (files transferred, bytes sent).

If rsync fails with "permission denied" on the server side, the
deployment path may not exist or futuner may not own it. Try
creating it via SSH first:

  sshpass -e ssh futuner@api.sillyrabbitmotorsport.com \
      'sudo mkdir -p ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud} && sudo chown futuner ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud}'

Then retry rsync. If still failing, STOP and surface the error.

==========================================================
PHASE 3 — Rebuild + restart Docker container on the server
==========================================================

Run remotely via SSH:

  sshpass -e ssh futuner@api.sillyrabbitmotorsport.com bash -lc "
    set -e
    cd ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud}
    docker-compose down
    docker-compose build --no-cache
    docker-compose up -d
    sleep 5
    docker-compose ps
  "

If `docker-compose ps` shows the container as "Up", PASS.

If "Exit" or missing, fetch the last 100 lines of logs:
  sshpass -e ssh futuner@api.sillyrabbitmotorsport.com \
      "cd ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud} && docker-compose logs srm-cloud --tail=100"
Surface the logs and STOP.

If docker-compose isn't installed on the server, STOP and tell me.

==========================================================
PHASE 4 — Verify /health
==========================================================

  curl -fsS https://api.sillyrabbitmotorsport.com/health

Expect JSON with "ok":true. If 502 / 503 / connection refused,
fetch container logs as above, surface, STOP.

==========================================================
PHASE 5 — Verify admin endpoint reachable (only if ADMIN_API_KEY set)
==========================================================

If ADMIN_API_KEY env var is non-empty:

  curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
       https://api.sillyrabbitmotorsport.com/admin/devices

Expect a JSON array (may be []). If 403, the local ADMIN_API_KEY
doesn't match the server. Surface that and STOP — no other action.

To find the actual server-side key:
  sshpass -e ssh futuner@api.sillyrabbitmotorsport.com \
      "cd ${CLOUD_SSH_PATH:-/home/futuner/srm-cloud} && docker-compose exec -T srm-cloud env | grep ADMIN_API_KEY"

If ADMIN_API_KEY is unset locally, SKIP this phase.

==========================================================
PHASE 6 — Report
==========================================================

Print:

  Cloud rsync — YYYY-MM-DD HH:MM
  ===============================
  Phase 1 pre-flight:   PASS
  Phase 2 rsync:        PASS — N files, M bytes
  Phase 3 rebuild:      PASS — container Up
  Phase 4 /health:      PASS
  Phase 5 admin probe:  PASS / SKIPPED
  
  Anomalies:
   - <anything that surprised you>

Append the report to ~/esp/obd/file-update-2026-05-07.md (today's
log; create if missing).

Hand back. Don't commit.

Proceed.
```

---

## What you do, in order

1. Open Terminal on your Mac.

2. Install sshpass if you haven't:

   ```
   brew install hudochenkov/sshpass/sshpass
   ```

3. Export your server password (only place it should live —
   ephemeral shell env var, not a file):

   ```
   export CLOUD_SSH_PASS='c20xh2!@#'
   ```

   Optional, only if you also know the admin key:

   ```
   export ADMIN_API_KEY='your-admin-key'
   ```

4. Open Claude Code in that same Terminal.

5. Paste the prompt body (everything inside the code fence above).

6. Walk away. Comes back with a report.

If Phase 1's connectivity test fails, the agent will tell you why
(typically wrong password or hostname unreachable). Most likely
cause: typo in `CLOUD_SSH_PASS`. Re-export with the correct value
and re-paste.

If Phase 3 fails on `docker-compose: command not found`, your server
doesn't have Docker installed yet. That's a separate setup step
(Docker install + docker-compose install), not handled by this
prompt. Tell me and I'll write that prompt.

If Phase 5 fails with 403, run the "find the actual server-side key"
command at the bottom of Phase 5 — it'll print the real value. Set
that as `ADMIN_API_KEY` locally and re-run.
