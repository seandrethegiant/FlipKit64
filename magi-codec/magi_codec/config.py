"""Config, secrets and data-directory handling for Magi Codec.

Everything secret (VAPID private key, intake token, push subscriptions, call
history) lives in a data directory OUTSIDE any served folder:

  * $MAGI_CODEC_DIR, if set
  * Windows: %LOCALAPPDATA%\\Magi\\codec
  * otherwise: ~/.magi/codec
"""
from __future__ import annotations

import base64
import json
import logging
import os
import secrets
import tempfile
import threading
from pathlib import Path

log = logging.getLogger("magi.codec")

DEFAULT_CONFIG = {
    # Shared secret for POST /api/codec/event (X-Magi-Token header).
    "intake_token": None,  # generated on first run
    # Web Push (VAPID). The private key is a PEM file next to this config.
    "vapid_private_key_file": "vapid_private.pem",
    "vapid_public_key": None,  # generated on first run
    # Must be a mailto: or https: URL. Push services use it to contact you.
    "vapid_subject": "mailto:magi@localhost",
    # Where the dashboard lives (the call page returns here when it ends).
    "dashboard_url": "/",
    "disk": {
        "enabled": True,
        "path": "D:\\" if os.name == "nt" else "/",
        "threshold_gb": 50,
        # Re-arm the alert once free space is back above threshold + margin.
        "rearm_margin_gb": 5,
        "interval_seconds": 300,
    },
    "camera_motion_cooldown_seconds": 300,
    "history_limit": 500,
}


def default_data_dir() -> Path:
    env = os.environ.get("MAGI_CODEC_DIR")
    if env:
        return Path(env)
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return Path(os.environ["LOCALAPPDATA"]) / "Magi" / "codec"
    return Path.home() / ".magi" / "codec"


_write_lock = threading.Lock()


def read_json(path: Path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return default
    except Exception:  # corrupt file: keep a copy, start fresh
        log.exception("codec: could not read %s, starting fresh", path)
        try:
            os.replace(path, str(path) + ".corrupt")
        except OSError:
            pass
        return default


def write_json(path: Path, data) -> None:
    """Atomic write (temp file + rename) so a crash never leaves half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with _write_lock:
        fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=path.name, suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            os.replace(tmp, path)
        except Exception:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise


def _merge(defaults: dict, current: dict) -> dict:
    out = dict(defaults)
    for k, v in current.items():
        if isinstance(v, dict) and isinstance(defaults.get(k), dict):
            out[k] = _merge(defaults[k], v)
        else:
            out[k] = v
    return out


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def generate_vapid_keys(pem_path: Path) -> str:
    """Create a P-256 key pair; write the private key as PEM, return the public key
    in the base64url uncompressed-point form browsers expect."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    pem_path.parent.mkdir(parents=True, exist_ok=True)
    with open(pem_path, "wb") as f:
        f.write(pem)
    try:
        os.chmod(pem_path, 0o600)
    except OSError:
        pass
    pub = key.public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint
    )
    return b64url(pub)


def load_config(data_dir: Path) -> dict:
    """Load config.json, creating it (and the VAPID keys / token) on first run."""
    data_dir.mkdir(parents=True, exist_ok=True)
    path = data_dir / "config.json"
    stored = read_json(path, {})
    cfg = _merge(DEFAULT_CONFIG, stored)
    changed = cfg != stored

    if not cfg.get("intake_token"):
        cfg["intake_token"] = secrets.token_urlsafe(32)
        changed = True
        log.info("codec: generated intake token in %s", path)

    pem = data_dir / cfg["vapid_private_key_file"]
    if not pem.exists() or not cfg.get("vapid_public_key"):
        cfg["vapid_public_key"] = generate_vapid_keys(pem)
        changed = True
        log.info("codec: generated VAPID keys in %s (existing subscriptions must re-enable)", data_dir)

    if changed:
        write_json(path, cfg)
    return cfg
