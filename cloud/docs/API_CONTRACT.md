# SRM Cloud — API Contract

> **Conventions are LOCKED.** Every endpoint across all phases conforms
> to the `## Conventions` section below; it does not drift. The
> `## Endpoint registry` and `## Backward-compat surface (frozen)`
> sections are templates filled in per phase as endpoints land. Changing
> a FROZEN row requires explicit Sean sign-off.

## Conventions

### Errors

All error responses use one envelope:
`{error: {code: string, message: string, details?: object}}`.

- `code` is a stable identifier; clients may switch on it.
  `message` is for humans and may evolve.
- 4xx codes documented per-endpoint in the row below.
- 5xx never leak internals; log server-side with a request_id,
  return `{error: {code: "internal", message: "...",
  details: {request_id: "..."}}}`.

### Auth
- Dongle endpoints: `Authorization: Bearer <token>` (existing
  convention from /api/v1/device/*).
- Customer endpoints: `Cookie: session=<token>` HttpOnly Secure
  SameSite=Lax. Token issued by /api/v1/user/auth/login.
- Admin endpoints: `X-Admin-Key: <ADMIN_API_KEY>` (existing).
- 401 = no/invalid credentials. 403 = valid creds but not
  authorized for this resource. **EXCEPT** for resources where
  existence is private (logs, vehicles, users belonging to other
  customers) — return 404 to avoid leaking existence.

### Privacy / disclosure
- Never confirm whether an email is registered (`/auth/reset/
  request` always 200, regardless).
- Never include the requesting user's other-account data in
  error payloads.
- VINs are logged at INFO level; passwords NEVER logged, even
  hashed.

### Content
- All request bodies: `Content-Type: application/json`.
- Log downloads (`/logs/{id}/download`): `Content-Type:
  application/gzip`, `Content-Encoding: gzip`,
  `Content-Disposition: attachment; filename="<vin>_<ts>.json.gz"`.
- Decompressed JSON view: `Content-Type: application/json`.

### Rate limiting
- Authenticated customer endpoints: per-user bucket,
  `RATE_LIMIT_USER_PER_MIN` requests/min.
- Auth endpoints (login, signup, reset/request, reset/confirm):
  per-IP bucket, `RATE_LIMIT_AUTH_PER_MIN`. Tighter limit per
  email on /reset/request to block enumeration.
- 429 response with `Retry-After` header.

### Versioning
- All customer + dongle endpoints prefixed with `/api/v1/`.
- Breaking changes go to `/api/v2/`; v1 endpoints stay alive
  during a deprecation window documented in this file.
- Admin endpoints are unversioned (internal) but their shape is
  documented here too.

## Endpoint registry

Filled in by CC per phase, grouped by file owner: auth.py,
vehicles.py, licenses.py, logs.py, views.py, admin.py.

> **Two deviations from the supplied signup template, flagged for
> Sean.** (1) The session is delivered ONLY as the HttpOnly cookie, not
> echoed as a `session_token` JSON field — returning it in the body
> would contradict the LOCKED `## Auth` convention (HttpOnly defeats the
> point if JS can read the token from the response). (2) `user_id` is the
> integer `users.id`, not a uuid — the PHASE 1 schema uses
> `INTEGER PRIMARY KEY AUTOINCREMENT`. Switching to uuid is a schema
> change; holding for sign-off rather than guessing.

### auth.py (PHASE 2)

#### `POST /api/v1/user/auth/signup`
- **Auth:** none
- **Request:** `{email: string, password: string}`
- **Success (201):** `{user_id: int, email: string, verified: bool}`
  + `Set-Cookie: session=<token>` (HttpOnly, SameSite=Lax, Secure in prod)
- **Errors:** 400 `email_invalid` · 400 `password_too_weak`
  · 409 `email_already_registered` · (429 `rate_limited` — bucket named,
  enforcement deferred)
- **Notes:** email lowercased/trimmed; sends a stubbed verification email
  (logged via `srm.email`, never the password). Signup is the one endpoint
  that legitimately discloses existence (can't register twice → 409).

#### `POST /api/v1/user/auth/login`
- **Auth:** none (establishes a session)
- **Request:** `{email: string, password: string}`
- **Success (200):** `{user_id, email, verified}` + session cookie
- **Errors:** 401 `invalid_credentials` (ONE generic error for both unknown
  email and wrong password — anti-enumeration, timing-equalized)
- **Notes:** transparently re-hashes the stored password if it's below the
  current KDF cost/scheme.

#### `POST /api/v1/user/auth/logout`
- **Auth:** session cookie (optional — idempotent)
- **Request:** none
- **Success (200):** `{ok: true}` — deletes the session row + clears the cookie
- **Errors:** none (no/stale cookie still 200)

#### `POST /api/v1/user/auth/reset/request`
- **Auth:** none
- **Request:** `{email: string}`
- **Success (200):** `{ok: true}` **ALWAYS**, regardless of whether the email
  is registered (LOCKED Privacy convention)
- **Errors:** none surfaced · (429 per-email bucket — deferred)
- **Notes:** if registered, mints a short-lived reset token + sends the
  stubbed email.

#### `POST /api/v1/user/auth/reset/confirm`
- **Auth:** none (bearer of the reset token)
- **Request:** `{token: string, password: string}`
- **Success (200):** `{ok: true}`
- **Errors:** 400 `password_too_weak` (validated first, so the failure mode
  is identical regardless of token validity) · 400 `invalid_token`
- **Notes:** token is single-use; on success ALL of that user's sessions are
  invalidated.

#### `POST /api/v1/user/auth/verify`
- **Auth:** none (bearer of the verify token)
- **Request:** `{token: string}`
- **Success (200):** `{ok: true}` — sets `users.verified = 1`
- **Errors:** 400 `invalid_token`
- **Notes:** token is single-use.

#### `GET /api/v1/user/auth/me`
- **Auth:** session cookie (required)
- **Success (200):** `{user_id, email, verified}`
- **Errors:** 401 `not_authenticated`
- **Notes:** resolves `get_current_user`; cheap "am I logged in" probe for the
  SSR/SPA. The dependency other customer modules import.

[CC: future phases append their blocks under a new `### <file>.py` heading.]

## Backward-compat surface (frozen)

These endpoints existed before PHASE 0. They MUST keep their
current request/response shape. Document the exact shape here
verbatim from src/main.py to prevent silent drift.

[CC: fill in per PHASE 0 audit — every existing endpoint gets a
row, marked FROZEN. Future PRs that change a FROZEN row require
explicit Sean sign-off.]
