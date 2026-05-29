"""
Password hashing for customer accounts (PHASE 2).

A salted, algorithm-tagged KDF behind a tiny interface, so the algorithm
can be swapped without touching call sites or breaking stored-hash
compatibility.

Current algorithm: PBKDF2-HMAC-SHA256 (Python stdlib — zero extra deps,
runs in the python:3.12-slim container and the local test env as-is).
argon2id was the handoff's stated preference; it stays a one-function
swap here once `argon2-cffi` is signed off as a runtime dependency (the
AUDIT "argon2 vs bcrypt" open question). Because stored hashes carry
their scheme tag, verify_password keeps validating old hashes across a
future swap, and callers can re-hash on next successful login
(needs_rehash) to migrate forward with no flag day.

Stored format (the single string kept in users.password_hash):
    pbkdf2_sha256$<iterations>$<salt_b64url>$<dk_b64url>
"""
from __future__ import annotations
import base64, hashlib, hmac, os, secrets
from typing import Optional

# Tunables — named + env-overridable per Rule 3 (never a bare literal).
PBKDF2_ALGO       = "sha256"
PBKDF2_ITERATIONS = int(os.environ.get("PBKDF2_ITERATIONS", "600000"))  # OWASP 2023 floor, PBKDF2-HMAC-SHA256
PBKDF2_SALT_BYTES = 16
_SCHEME           = "pbkdf2_sha256"

# Password policy. Enforced at the API edge for a friendly 400; MAX is
# also a DoS guard since the KDF processes the whole input.
PASSWORD_MIN_LEN = int(os.environ.get("PASSWORD_MIN_LEN", "10"))
PASSWORD_MAX_LEN = int(os.environ.get("PASSWORD_MAX_LEN", "1024"))


def _b64e(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _b64d(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def hash_password(password: str, *, iterations: Optional[int] = None) -> str:
    """Return a self-describing hash string for `password`."""
    iters = iterations or PBKDF2_ITERATIONS
    salt = secrets.token_bytes(PBKDF2_SALT_BYTES)
    dk = hashlib.pbkdf2_hmac(PBKDF2_ALGO, password.encode("utf-8"), salt, iters)
    return f"{_SCHEME}${iters}${_b64e(salt)}${_b64e(dk)}"


def verify_password(password: str, stored: str) -> bool:
    """Constant-time verify. Returns False (never raises) on any
    malformed/unknown stored hash, so a corrupt row can't 500 a login."""
    try:
        scheme, iters_s, salt_b64, dk_b64 = stored.split("$")
    except (ValueError, AttributeError):
        return False
    if scheme != _SCHEME:
        return False  # future: dispatch argon2id (and other schemes) here
    try:
        iters = int(iters_s)
        salt = _b64d(salt_b64)
        expected = _b64d(dk_b64)
    except Exception:
        return False
    dk = hashlib.pbkdf2_hmac(PBKDF2_ALGO, password.encode("utf-8"), salt, iters)
    return hmac.compare_digest(dk, expected)


def needs_rehash(stored: str) -> bool:
    """True if `stored` isn't the current scheme + iteration count, so the
    caller can transparently re-hash on the next successful login."""
    try:
        scheme, iters_s, _, _ = stored.split("$")
    except (ValueError, AttributeError):
        return True
    try:
        return scheme != _SCHEME or int(iters_s) < PBKDF2_ITERATIONS
    except ValueError:
        return True
