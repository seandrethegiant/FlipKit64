"""Magi Codec: phone "Codec call" alerts for the Magi dashboard.

Usage in Magi (Flask):

    from magi_codec import init_codec
    init_codec(app)

then add the panel to the dashboard template (see README).
"""
from __future__ import annotations

import logging
import os

from .service import CodecService, EventError  # noqa: F401

log = logging.getLogger("magi.codec")
__all__ = ["init_codec", "CodecService", "EventError"]


def init_codec(app, data_dir=None, static_dir=None, callers_file=None, start_disk_monitor=True):
    """Register the Codec routes on a Flask app. Returns the CodecService.

    Failures here are logged and swallowed: Magi keeps running without Codec.
    """
    try:
        from .routes import create_blueprint

        service = CodecService(data_dir=data_dir, static_dir=static_dir, callers_file=callers_file)
        app.register_blueprint(create_blueprint(service))
        app.extensions["magi_codec"] = service
        log.info("codec: ready (data in %s)", service.data_dir)
    except Exception:
        log.exception("codec: failed to start; Magi continues without Codec alerts")
        return None

    disk = service.config.get("disk") or {}
    # With Flask's debug reloader the app is built twice; only the child serves.
    reloader_parent = app.debug and os.environ.get("WERKZEUG_RUN_MAIN") != "true"
    if start_disk_monitor and disk.get("enabled") and not reloader_parent and not app.testing:
        try:
            from .disk_monitor import DiskMonitor

            mon = DiskMonitor(service)
            mon.start()
            service.disk_monitor = mon
        except Exception:
            log.exception("codec: disk monitor failed to start")
    return service
