# SPDX-License-Identifier: MIT — part of frame-updater by sasaken1102r, shipped under the host app's MIT license
"""Thin asyncio wrapper around frame-update.sh, for Python apps (frame-jp-keyboard's injector).

Copied from the frame-updater repository (see UPSTREAM next to the copy); standard library only.
Every call runs the script and returns its one-line JSON answer as a dict (see frame-updater's
README.md for the fields). Example:

    updater = Updater(script, "frame-jp-keyboard", "sasaken1102r/frame-jp-keyboard", "0.5.1",
                      "frame-jp-keyboard-{version}.tar.gz")
    answer = await updater.check()          # at start, then hourly: GitHub is asked at most daily
    if answer["status"] == "update-available" and answer.get("installable"):
        summary = answer.get("notes_ja") or answer.get("notes", "")  # its summary on a Japanese screen ("" = none)
        await updater.install()             # after the user pressed the button; returns at once
    progress = await updater.state()        # poll while it runs: running / done / failed
"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

CHECK_TIMEOUT_SECONDS = 90.0
START_TIMEOUT_SECONDS = 30.0


class Updater:
    """Runs frame-update.sh for one app."""

    def __init__(self, script: str | Path, app: str, repo: str, current: str, asset: str,
                 default_install_args: tuple[str, ...] = ()) -> None:
        """
        script: path of the installed frame-update.sh
        app: app name (cache/config folder and systemd unit name)
        repo: GitHub "owner/name"
        current: version of the running app
        asset: release file name with {version} for the version
        default_install_args: install.sh options when ~/.config/<app>/install-args is missing
        """
        self.script = str(script)
        self.app = app
        self.repo = repo
        self.current = current
        self.asset = asset
        self.default_install_args = tuple(default_install_args)
        cache = os.environ.get("XDG_CACHE_HOME", "")
        self.cache_dir = (Path(cache) if cache.startswith("/") else Path.home() / ".cache") / app
        self.log_path = self.cache_dir / "update.log"

    def _base(self) -> list[str]:
        return ["sh", self.script, "--app", self.app, "--repo", self.repo, "--current", self.current,
                "--asset", self.asset]

    async def _run(self, args: list[str], timeout: float, failed: dict) -> dict:
        """Run the script and parse the last line of its output; on any failure return `failed`."""
        try:
            process = await asyncio.create_subprocess_exec(
                *args, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE)
        except OSError as error:
            return {**failed, "error": "spawn-failed", "message": str(error)}
        try:
            out, _ = await asyncio.wait_for(process.communicate(), timeout)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()
            return {**failed, "error": "script-failed", "message": "frame-update.sh timed out"}
        lines = [line for line in out.decode("utf-8", "replace").splitlines() if line.strip()]
        try:
            answer = json.loads(lines[-1])
            if isinstance(answer, dict):
                return answer
        except (IndexError, ValueError):
            pass
        return {**failed, "error": "script-failed", "message": "no answer from frame-update.sh"}

    async def check(self, force: bool = False) -> dict:
        """Check for a newer release: {"status": "up-to-date" | "update-available" | "error", ...}.
        "update-available" also has "notes" and "notes_ja": the release's summary in English and Japanese
        (plain text on one line, "" if the release text has none)."""
        args = self._base() + (["--force"] if force else []) + ["check"]
        return await self._run(args, CHECK_TIMEOUT_SECONDS, {"status": "error"})

    async def install(self) -> dict:
        """Start installing the newest release in the systemd user unit <app>-update; returns at once.
        The unit restarts this app's service, so the rest is followed through state()."""
        args = self._base()
        for arg in self.default_install_args:
            args += ["--install-arg", arg]
        args += ["--detach", "install"]
        return await self._run(args, START_TIMEOUT_SECONDS, {"state": "failed"})

    async def state(self) -> dict:
        """Progress of the last install: {"state": "idle" | "running" | "done" | "failed", ...}."""
        return await self._run(["sh", self.script, "--app", self.app, "status"], START_TIMEOUT_SECONDS,
                               {"state": "failed"})

    def dismiss(self) -> None:
        """Forget a finished or failed install (removes the state file)."""
        try:
            (self.cache_dir / "update-state.json").unlink()
        except FileNotFoundError:
            pass
