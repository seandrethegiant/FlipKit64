"""Background free-space check. Fires one disk_low call per threshold crossing."""
from __future__ import annotations

import logging
import shutil
import threading

log = logging.getLogger("magi.codec")

GB = 1024 ** 3


def check_once(service) -> str:
    """Run one check. Returns 'fired', 'low', 'ok', 'rearmed' or 'error'."""
    disk = service.config.get("disk") or {}
    path = disk.get("path") or "/"
    threshold = float(disk.get("threshold_gb", 50))
    margin = float(disk.get("rearm_margin_gb", 5))
    try:
        free_gb = shutil.disk_usage(path).free / GB
    except Exception:
        log.exception("codec: disk check failed for %s", path)
        return "error"

    if free_gb < threshold:
        if not service.disk_low_armed():
            return "low"
        result = service.handle_event({
            "type": "disk_low",
            "title": "Storage running low",
            "message": f"{path} has {free_gb:.1f} GB free. Alert threshold is {threshold:g} GB. "
                       f"Clear some space before the next conversion.",
        })
        return "fired" if not result.get("suppressed") else "low"
    if free_gb >= threshold + margin and not service.disk_low_armed():
        service.rearm_disk_low()
        return "rearmed"
    return "ok"


class DiskMonitor(threading.Thread):
    def __init__(self, service):
        super().__init__(name="magi-codec-disk", daemon=True)
        self.service = service
        self._halt = threading.Event()

    def run(self):
        interval = float((self.service.config.get("disk") or {}).get("interval_seconds", 300))
        log.info("codec: disk monitor started (every %ss)", int(interval))
        while not self._halt.is_set():
            try:
                check_once(self.service)
            except Exception:  # never let the thread die
                log.exception("codec: disk monitor error")
            self._halt.wait(interval)

    def stop(self):
        self._halt.set()
