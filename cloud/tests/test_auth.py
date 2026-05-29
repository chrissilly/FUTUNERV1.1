"""
PHASE 2 customer-auth tests: signup / login / logout / reset / verify and
the session cookie + get_current_user dependency.

Uses the function-scoped `client` fixture from conftest.py (fresh DB per
test, PBKDF2 cost knocked down for speed). Email + reset/verify tokens are
read straight from the test DB via src.main.db() — the fixture has pointed
DB_PATH at the per-test tempdir for the duration of the test.
"""
import logging

import src.main as m

AUTH = "/api/v1/user/auth"
GOOD_PW = "correct horse battery staple"   # well over PASSWORD_MIN_LEN


def _latest_token(purpose: str) -> str:
    """Most-recently-created user_tokens row of a given purpose."""
    conn = m.db()
    try:
        row = conn.execute(
            "SELECT token FROM user_tokens WHERE purpose = ? "
            "ORDER BY created_at DESC, rowid DESC LIMIT 1",
            (purpose,),
        ).fetchone()
    finally:
        conn.close()
    assert row is not None, f"no {purpose} token was issued"
    return row["token"]


def _user_row(email: str):
    conn = m.db()
    try:
        return conn.execute("SELECT * FROM users WHERE email = ?", (email,)).fetchone()
    finally:
        conn.close()


# --------------------------------------------------------------------------
# signup
# --------------------------------------------------------------------------
def test_signup_happy_sets_cookie_and_me_works(client):
    r = client.post(f"{AUTH}/signup", json={"email": "a@b.com", "password": GOOD_PW})
    assert r.status_code == 201, r.text
    body = r.json()
    assert body["email"] == "a@b.com"
    assert body["verified"] is False
    assert isinstance(body["user_id"], int)
    # session token is the HttpOnly cookie ONLY — never echoed in the body
    assert "session_token" not in body
    assert "password" not in body and "password_hash" not in body
    assert m.SESSION_COOKIE_NAME in r.cookies

    # the cookie is now in the client jar → /me resolves the user
    r = client.get(f"{AUTH}/me")
    assert r.status_code == 200
    assert r.json()["email"] == "a@b.com"


def test_signup_normalizes_email(client):
    r = client.post(f"{AUTH}/signup", json={"email": "  MixedCase@B.COM ", "password": GOOD_PW})
    assert r.status_code == 201, r.text
    assert r.json()["email"] == "mixedcase@b.com"


def test_signup_email_invalid(client):
    for bad in ["", "nope", "no@domain", "a b@c.com", "@c.com"]:
        r = client.post(f"{AUTH}/signup", json={"email": bad, "password": GOOD_PW})
        assert r.status_code == 400, (bad, r.text)
        assert r.json()["error"]["code"] == "email_invalid"


def test_signup_password_too_weak(client):
    r = client.post(f"{AUTH}/signup", json={"email": "x@y.com", "password": "short"})
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "password_too_weak"


def test_signup_duplicate_email_409(client):
    client.post(f"{AUTH}/signup", json={"email": "dup@b.com", "password": GOOD_PW})
    r = client.post(f"{AUTH}/signup", json={"email": "dup@b.com", "password": GOOD_PW})
    assert r.status_code == 409
    assert r.json()["error"]["code"] == "email_already_registered"


def test_signup_password_never_stored_plaintext(client):
    client.post(f"{AUTH}/signup", json={"email": "secret@b.com", "password": GOOD_PW})
    row = _user_row("secret@b.com")
    assert row["password_hash"] != GOOD_PW
    assert GOOD_PW not in row["password_hash"]
    assert row["password_hash"].startswith("pbkdf2_sha256$")


# --------------------------------------------------------------------------
# login
# --------------------------------------------------------------------------
def test_login_happy(client):
    client.post(f"{AUTH}/signup", json={"email": "log@b.com", "password": GOOD_PW})
    client.post(f"{AUTH}/logout")  # drop the signup session/cookie first
    r = client.post(f"{AUTH}/login", json={"email": "log@b.com", "password": GOOD_PW})
    assert r.status_code == 200, r.text
    assert r.json()["email"] == "log@b.com"
    assert m.SESSION_COOKIE_NAME in r.cookies
    assert client.get(f"{AUTH}/me").status_code == 200


def test_login_wrong_password_401_generic(client):
    client.post(f"{AUTH}/signup", json={"email": "wp@b.com", "password": GOOD_PW})
    r = client.post(f"{AUTH}/login", json={"email": "wp@b.com", "password": "wrongwrongwrong"})
    assert r.status_code == 401
    assert r.json()["error"]["code"] == "invalid_credentials"


def test_login_unknown_email_same_error(client):
    # anti-enumeration: unknown email returns the SAME code as wrong password
    r = client.post(f"{AUTH}/login", json={"email": "ghost@b.com", "password": GOOD_PW})
    assert r.status_code == 401
    assert r.json()["error"]["code"] == "invalid_credentials"


# --------------------------------------------------------------------------
# logout
# --------------------------------------------------------------------------
def test_logout_invalidates_session(client):
    client.post(f"{AUTH}/signup", json={"email": "out@b.com", "password": GOOD_PW})
    assert client.get(f"{AUTH}/me").status_code == 200
    r = client.post(f"{AUTH}/logout")
    assert r.status_code == 200 and r.json()["ok"] is True
    assert client.get(f"{AUTH}/me").status_code == 401


def test_logout_idempotent_without_cookie(client):
    r = client.post(f"{AUTH}/logout")  # no session at all
    assert r.status_code == 200 and r.json()["ok"] is True


# --------------------------------------------------------------------------
# get_current_user / sessions
# --------------------------------------------------------------------------
def test_me_requires_auth(client):
    r = client.get(f"{AUTH}/me")
    assert r.status_code == 401
    assert r.json()["error"]["code"] == "not_authenticated"


def test_me_rejects_bogus_cookie(client):
    client.cookies.set(m.SESSION_COOKIE_NAME, "not-a-real-session-token")
    r = client.get(f"{AUTH}/me")
    assert r.status_code == 401
    assert r.json()["error"]["code"] == "not_authenticated"


# --------------------------------------------------------------------------
# verify
# --------------------------------------------------------------------------
def test_verify_marks_user_verified(client):
    client.post(f"{AUTH}/signup", json={"email": "ver@b.com", "password": GOOD_PW})
    assert _user_row("ver@b.com")["verified"] == 0
    token = _latest_token("verify")
    r = client.post(f"{AUTH}/verify", json={"token": token})
    assert r.status_code == 200 and r.json()["ok"] is True
    assert _user_row("ver@b.com")["verified"] == 1


def test_verify_token_is_single_use(client):
    client.post(f"{AUTH}/signup", json={"email": "once@b.com", "password": GOOD_PW})
    token = _latest_token("verify")
    assert client.post(f"{AUTH}/verify", json={"token": token}).status_code == 200
    r = client.post(f"{AUTH}/verify", json={"token": token})  # replay
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "invalid_token"


def test_verify_bad_token_400(client):
    r = client.post(f"{AUTH}/verify", json={"token": "garbage"})
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "invalid_token"


# --------------------------------------------------------------------------
# password reset
# --------------------------------------------------------------------------
def test_reset_request_always_200_even_unknown(client):
    r = client.post(f"{AUTH}/reset/request", json={"email": "nobody@b.com"})
    assert r.status_code == 200 and r.json()["ok"] is True
    # no token should have been minted for a non-existent user
    conn = m.db()
    try:
        n = conn.execute("SELECT COUNT(*) c FROM user_tokens WHERE purpose='reset'").fetchone()["c"]
    finally:
        conn.close()
    assert n == 0


def test_reset_full_flow_changes_password_and_kills_sessions(client):
    client.post(f"{AUTH}/signup", json={"email": "rst@b.com", "password": GOOD_PW})
    # request reset → token minted
    assert client.post(f"{AUTH}/reset/request", json={"email": "rst@b.com"}).status_code == 200
    token = _latest_token("reset")

    new_pw = "a brand new much longer password"
    r = client.post(f"{AUTH}/reset/confirm", json={"token": token, "password": new_pw})
    assert r.status_code == 200, r.text

    # the signup session was invalidated by the reset
    assert client.get(f"{AUTH}/me").status_code == 401
    # old password no longer works; new one does
    assert client.post(f"{AUTH}/login", json={"email": "rst@b.com", "password": GOOD_PW}).status_code == 401
    assert client.post(f"{AUTH}/login", json={"email": "rst@b.com", "password": new_pw}).status_code == 200


def test_reset_confirm_bad_token_400(client):
    r = client.post(f"{AUTH}/reset/confirm", json={"token": "nope", "password": GOOD_PW})
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "invalid_token"


def test_reset_confirm_weak_password_checked_first(client):
    # weak password fails as password_too_weak regardless of token validity
    r = client.post(f"{AUTH}/reset/confirm", json={"token": "whatever", "password": "x"})
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "password_too_weak"


def test_reset_token_single_use(client):
    client.post(f"{AUTH}/signup", json={"email": "rs2@b.com", "password": GOOD_PW})
    client.post(f"{AUTH}/reset/request", json={"email": "rs2@b.com"})
    token = _latest_token("reset")
    new_pw = "another sufficiently long password"
    assert client.post(f"{AUTH}/reset/confirm", json={"token": token, "password": new_pw}).status_code == 200
    r = client.post(f"{AUTH}/reset/confirm", json={"token": token, "password": new_pw})
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "invalid_token"


# --------------------------------------------------------------------------
# error envelope + privacy
# --------------------------------------------------------------------------
def test_error_envelope_shape(client):
    r = client.post(f"{AUTH}/signup", json={"email": "bad", "password": GOOD_PW})
    body = r.json()
    assert set(body.keys()) == {"error"}
    assert set(body["error"].keys()) == {"code", "message"}  # no details when None
    assert isinstance(body["error"]["message"], str) and body["error"]["message"]


def test_password_never_logged(client, caplog):
    caplog.set_level(logging.INFO)
    client.post(f"{AUTH}/signup", json={"email": "qa@b.com", "password": GOOD_PW})
    client.post(f"{AUTH}/reset/request", json={"email": "qa@b.com"})
    # the stub email logger fired (verify + reset) but never with the password
    joined = "\n".join(rec.getMessage() for rec in caplog.records)
    assert "srm.email" in {rec.name for rec in caplog.records} or "EMAIL ->" in joined
    assert GOOD_PW not in joined
