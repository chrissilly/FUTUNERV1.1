"""
Customer authentication + session endpoints (PHASE 2).

Mounted under /api/v1/user/auth/* and included by src/main.py at the
BOTTOM of that module (the names below — db, now_iso, ApiError, the
SESSION_* / *_TOKEN_TTL_* constants — are all defined in main before the
include line runs, which is what breaks the import cycle).

Design notes
------------
* Sessions are server-side (handoff D3): the cookie holds an opaque
  token, the `sessions` row is authoritative. Logout = DELETE the row;
  password reset = DELETE every row for that user. Real revocation, not
  "wait for a JWT to expire".
* Passwords go through src/passwords.py (algorithm-tagged KDF, swappable).
  We NEVER log or return a password or hash (API_CONTRACT ## Privacy).
* Anti-enumeration is deliberate and matches the LOCKED contract:
    - /login returns one generic 401 for both "no such email" and "wrong
      password", and runs a dummy verify when the email is unknown so the
      response time doesn't leak which emails are registered.
    - /reset/request ALWAYS returns 200 — it never reveals whether an
      email is on file.
  /signup is the one place existence is legitimately disclosed (you
  cannot register the same email twice → 409).
* Email + Stripe are STUBBED per the handoff: send_email_stub() only logs
  the message it would send. No SMTP/SES wiring here.
"""
from __future__ import annotations
import logging
import re
import secrets
import sqlite3
from typing import Optional

from fastapi import APIRouter, Cookie, Depends, Response
from pydantic import BaseModel

from . import main as _main   # late-bound config (see _set_session_cookie)
from .main import (
    ApiError,
    db,
    now_iso,
    SESSION_COOKIE_NAME,
    SESSION_TTL_SECONDS,
    VERIFY_TOKEN_TTL_SECONDS,
    RESET_TOKEN_TTL_SECONDS,
)
from .passwords import (
    hash_password,
    verify_password,
    needs_rehash,
    PASSWORD_MIN_LEN,
    PASSWORD_MAX_LEN,
)

router = APIRouter(prefix="/api/v1/user/auth", tags=["customer-auth"])

# Pragmatic email shape check (NOT full RFC 5322 — that's a known rabbit
# hole). Rejects whitespace and requires a single @ with a dotted domain;
# anything past this is validated by the verification email round-trip.
EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")

# Email transport is stubbed (handoff). Route it through a named logger so a
# deployment can capture/forward "srm.email" without touching app logs.
_email_log = logging.getLogger("srm.email")

# Token `purpose` values — named, not bare literals (Rule 3).
PURPOSE_VERIFY = "verify"
PURPOSE_RESET  = "reset"

# Lazily-computed dummy hash for the login timing-equalizer (see _login).
_DUMMY_HASH: Optional[str] = None


# ---------------------------------------------------------------------------
# Request bodies. Fields are Optional so a missing/!str field becomes our
# LOCKED {error:{...}} envelope (via _check_*), not FastAPI's raw 422.
# ---------------------------------------------------------------------------
class SignupBody(BaseModel):
    email: Optional[str] = None
    password: Optional[str] = None


class LoginBody(BaseModel):
    email: Optional[str] = None
    password: Optional[str] = None


class ResetRequestBody(BaseModel):
    email: Optional[str] = None


class ResetConfirmBody(BaseModel):
    token: Optional[str] = None
    password: Optional[str] = None


class VerifyBody(BaseModel):
    token: Optional[str] = None


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def send_email_stub(to: str, subject: str, body: str) -> None:
    """STUB — the handoff says 'a function that LOGS what it would send, do
    not wire SMTP/SES'. Never pass a password or hash in `body`; only opaque
    tokens / links (API_CONTRACT: passwords NEVER logged, even hashed)."""
    _email_log.info("EMAIL -> %s | %s | %s", to, subject, body)


def _normalize_email(raw: Optional[str]) -> str:
    return (raw or "").strip().lower()


def _check_email(raw: Optional[str]) -> str:
    email = _normalize_email(raw)
    if not email or not EMAIL_RE.match(email):
        raise ApiError(400, "email_invalid", "Provide a valid email address.")
    return email


def _check_password(raw: Optional[str]) -> str:
    if not isinstance(raw, str) or not (PASSWORD_MIN_LEN <= len(raw) <= PASSWORD_MAX_LEN):
        raise ApiError(
            400, "password_too_weak",
            f"Password must be {PASSWORD_MIN_LEN}-{PASSWORD_MAX_LEN} characters.",
        )
    return raw


def _dummy_hash() -> str:
    """A real hash of a throwaway value, computed once at current cost. Used
    so /login spends ~the same time whether or not the email exists."""
    global _DUMMY_HASH
    if _DUMMY_HASH is None:
        _DUMMY_HASH = hash_password("x" * PASSWORD_MIN_LEN)
    return _DUMMY_HASH


def _issue_session(conn: sqlite3.Connection, user_id: int) -> str:
    token = secrets.token_urlsafe(32)
    conn.execute(
        "INSERT INTO sessions (token, user_id, expires_at, created_at) "
        "VALUES (?, ?, ?, ?)",
        (token, user_id, now_iso(SESSION_TTL_SECONDS), now_iso()),
    )
    return token


def _issue_link_token(conn: sqlite3.Connection, user_id: int, purpose: str,
                      ttl_seconds: int) -> str:
    token = secrets.token_urlsafe(32)
    conn.execute(
        "INSERT INTO user_tokens (token, user_id, purpose, expires_at, created_at) "
        "VALUES (?, ?, ?, ?, ?)",
        (token, user_id, purpose, now_iso(ttl_seconds), now_iso()),
    )
    return token


def _set_session_cookie(response: Response, token: str) -> None:
    # Secure read live from the module (not import-time) so the test harness
    # can flip it off for http TestClient via monkeypatch.setattr(main, ...).
    response.set_cookie(
        key=SESSION_COOKIE_NAME, value=token, max_age=SESSION_TTL_SECONDS,
        httponly=True, secure=_main.SESSION_COOKIE_SECURE, samesite="lax", path="/",
    )


def _public_user(row: sqlite3.Row) -> dict:
    """The non-secret view of a user row (never password_hash)."""
    return {"user_id": row["id"], "email": row["email"], "verified": bool(row["verified"])}


# ---------------------------------------------------------------------------
# Dependency: resolve the logged-in user from the session cookie. Other
# customer modules (vehicles/licenses/logs/views) import this.
# ---------------------------------------------------------------------------
def get_current_user(
    session: Optional[str] = Cookie(default=None, alias=SESSION_COOKIE_NAME),
) -> sqlite3.Row:
    if not session:
        raise ApiError(401, "not_authenticated", "Login required.")
    conn = db()
    try:
        row = conn.execute(
            "SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id "
            "WHERE s.token = ? AND s.expires_at > ?",
            (session, now_iso()),
        ).fetchone()
    finally:
        conn.close()
    if row is None:
        # Covers unknown token AND expired session — same 401 either way.
        raise ApiError(401, "not_authenticated", "Invalid or expired session.")
    return row


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------
@router.post("/signup", status_code=201)
async def signup(body: SignupBody, response: Response):
    email = _check_email(body.email)
    password = _check_password(body.password)
    pw_hash = hash_password(password)
    conn = db()
    try:
        try:
            cur = conn.execute(
                "INSERT INTO users (email, password_hash, verified, created_at) "
                "VALUES (?, ?, 0, ?)",
                (email, pw_hash, now_iso()),
            )
        except sqlite3.IntegrityError:
            # UNIQUE(email). Signup is the ONE endpoint where existence is
            # legitimately disclosed (you can't register an email twice).
            raise ApiError(409, "email_already_registered",
                           "That email is already registered.")
        user_id = cur.lastrowid
        verify_token = _issue_link_token(conn, user_id, PURPOSE_VERIFY,
                                         VERIFY_TOKEN_TTL_SECONDS)
        session_token = _issue_session(conn, user_id)
        conn.commit()
    finally:
        conn.close()
    send_email_stub(email, "Verify your SRM account",
                    f"Verify your account: /api/v1/user/auth/verify?token={verify_token}")
    # Cookie-based session (D3 + LOCKED Auth convention) — the session token
    # is set ONLY as an HttpOnly cookie, never echoed in the JSON body.
    _set_session_cookie(response, session_token)
    return {"user_id": user_id, "email": email, "verified": False}


@router.post("/login")
async def login(body: LoginBody, response: Response):
    email = _normalize_email(body.email)
    password = body.password if isinstance(body.password, str) else ""
    conn = db()
    try:
        row = conn.execute("SELECT * FROM users WHERE email = ?", (email,)).fetchone()
        # Equalize work whether or not the email exists, then return ONE
        # generic error for both cases (anti-enumeration).
        stored = row["password_hash"] if row else _dummy_hash()
        ok = verify_password(password, stored)
        if not row or not ok:
            raise ApiError(401, "invalid_credentials", "Email or password is incorrect.")
        # Transparent upgrade if the stored hash is below current cost/scheme.
        if needs_rehash(stored):
            conn.execute("UPDATE users SET password_hash = ? WHERE id = ?",
                         (hash_password(password), row["id"]))
        session_token = _issue_session(conn, row["id"])
        conn.commit()
        out = _public_user(row)
    finally:
        conn.close()
    _set_session_cookie(response, session_token)
    return out


@router.post("/logout")
async def logout(
    response: Response,
    session: Optional[str] = Cookie(default=None, alias=SESSION_COOKIE_NAME),
):
    # Idempotent: no/stale cookie still returns 200 with the cookie cleared.
    if session:
        conn = db()
        try:
            conn.execute("DELETE FROM sessions WHERE token = ?", (session,))
            conn.commit()
        finally:
            conn.close()
    response.delete_cookie(key=SESSION_COOKIE_NAME, path="/")
    return {"ok": True}


@router.post("/reset/request")
async def reset_request(body: ResetRequestBody):
    email = _normalize_email(body.email)
    # ALWAYS 200 regardless of whether the email is registered (LOCKED
    # Privacy convention). Only do work + send the stub mail if it exists.
    if email and EMAIL_RE.match(email):
        conn = db()
        sent_token: Optional[str] = None
        try:
            row = conn.execute("SELECT id FROM users WHERE email = ?", (email,)).fetchone()
            if row:
                sent_token = _issue_link_token(conn, row["id"], PURPOSE_RESET,
                                               RESET_TOKEN_TTL_SECONDS)
                conn.commit()
        finally:
            conn.close()
        if sent_token:
            send_email_stub(
                email, "Reset your SRM password",
                f"Reset your password: /api/v1/user/auth/reset/confirm?token={sent_token}")
    return {"ok": True}


@router.post("/reset/confirm")
async def reset_confirm(body: ResetConfirmBody):
    # Validate the new password BEFORE touching the token: a weak password
    # fails the same way regardless of token validity, leaking nothing.
    new_password = _check_password(body.password)
    token = body.token if isinstance(body.token, str) else ""
    if not token:
        raise ApiError(400, "invalid_token", "Invalid or expired reset link.")
    conn = db()
    try:
        row = conn.execute(
            "SELECT user_id FROM user_tokens "
            "WHERE token = ? AND purpose = ? AND used_at IS NULL AND expires_at > ?",
            (token, PURPOSE_RESET, now_iso()),
        ).fetchone()
        if row is None:
            raise ApiError(400, "invalid_token", "Invalid or expired reset link.")
        conn.execute("UPDATE users SET password_hash = ? WHERE id = ?",
                     (hash_password(new_password), row["user_id"]))
        # user_tokens PK is `token` (no `id` column) — key the single-use mark on it.
        conn.execute("UPDATE user_tokens SET used_at = ? WHERE token = ?",
                     (now_iso(), token))
        # A password reset logs out every existing session for that user.
        conn.execute("DELETE FROM sessions WHERE user_id = ?", (row["user_id"],))
        conn.commit()
    finally:
        conn.close()
    return {"ok": True}


@router.post("/verify")
async def verify(body: VerifyBody):
    token = body.token if isinstance(body.token, str) else ""
    if not token:
        raise ApiError(400, "invalid_token", "Invalid or expired verification link.")
    conn = db()
    try:
        row = conn.execute(
            "SELECT user_id FROM user_tokens "
            "WHERE token = ? AND purpose = ? AND used_at IS NULL AND expires_at > ?",
            (token, PURPOSE_VERIFY, now_iso()),
        ).fetchone()
        if row is None:
            raise ApiError(400, "invalid_token", "Invalid or expired verification link.")
        conn.execute("UPDATE users SET verified = 1 WHERE id = ?", (row["user_id"],))
        conn.execute("UPDATE user_tokens SET used_at = ? WHERE token = ?",
                     (now_iso(), token))
        conn.commit()
    finally:
        conn.close()
    return {"ok": True}


@router.get("/me")
async def me(user: sqlite3.Row = Depends(get_current_user)):
    """Current session's user — exercises get_current_user and gives the
    SSR/SPA a cheap 'am I logged in' probe."""
    return _public_user(user)
