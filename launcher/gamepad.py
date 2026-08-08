"""Optional gamepad navigation for the launcher.

The game itself already handles controllers — the gb-recompiled runtime binds
SDL_GameController directly and has remapping plus per-brand button labels in
its Esc menu. This module only covers the launcher window, so you can pick a
game and start it without reaching for a keyboard.

pygame (SDL2 under the hood) is an optional dependency. If it isn't installed,
`GamepadBridge.available` is False and the launcher runs keyboard/mouse only —
nothing here is load-bearing.

    pip install pygame
"""
from __future__ import annotations

import os
import sys

from PySide6.QtCore import QObject, QTimer, Signal

# pygame talks to SDL, which will happily try to open a window and an audio
# device we have no use for. Qt owns the actual UI, so point SDL at its null
# drivers before the import. This only affects SDL, never Qt.
os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

try:
    import pygame

    _HAVE_PYGAME = True
except ImportError:  # pragma: no cover - depends on the user's environment
    pygame = None  # type: ignore[assignment]
    _HAVE_PYGAME = False


AXIS_DEADZONE = 0.55
# Held-direction auto-repeat, in milliseconds: a longer wait before the first
# repeat so a deliberate single nudge moves exactly one row.
REPEAT_DELAY_MS = 420
REPEAT_RATE_MS = 130
POLL_MS = 16

# SDL_GameController's canonical layout. Face button 0 is the bottom one
# ("A" on Xbox, cross on PlayStation, B on a Nintendo pad), which is what
# every one of those platforms uses for confirm.
BTN_ACCEPT = 0
BTN_BACK = 1
BTN_START = 7
HAT_LEFT, HAT_RIGHT = (-1, 0), (1, 0)


class GamepadBridge(QObject):
    """Polls the first connected pad and emits abstract navigation events."""

    moved = Signal(int, int)   # dx, dy in {-1, 0, 1}
    accepted = Signal()
    cancelled = Signal()
    status = Signal(str)       # human-readable pad name, or "" when gone

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.available = _HAVE_PYGAME
        self._pad = None
        self._name = ""
        self._buttons: dict[int, bool] = {}
        self._dir = (0, 0)
        self._held_ms = 0
        self._fired_ms = 0

        if not self.available:
            return

        try:
            pygame.init()
            pygame.joystick.init()
        except Exception as exc:  # noqa: BLE001 - pygame raises broadly
            print(f"[launcher] gamepad disabled: {exc}", file=sys.stderr)
            self.available = False
            return

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(POLL_MS)

    # -- device management ------------------------------------------------

    def _ensure_pad(self) -> bool:
        if self._pad is not None:
            return True
        if pygame.joystick.get_count() == 0:
            return False
        try:
            pad = pygame.joystick.Joystick(0)
            pad.init()
        except Exception:  # noqa: BLE001
            return False
        self._pad = pad
        self._name = pad.get_name()
        self._buttons.clear()
        self.status.emit(self._name)
        print(f"[launcher] gamepad: {self._name}", file=sys.stderr)
        return True

    def _drop_pad(self) -> None:
        if self._pad is not None:
            self._pad = None
            self._name = ""
            self._dir = (0, 0)
            self.status.emit("")

    # -- polling ----------------------------------------------------------

    def _poll(self) -> None:
        try:
            pygame.event.pump()
        except Exception:  # noqa: BLE001
            return

        if not self._ensure_pad():
            self._drop_pad()
            return

        try:
            dx, dy = self._read_direction()
            self._pump_direction(dx, dy)
            self._pump_buttons()
        except Exception:  # noqa: BLE001 - pad unplugged mid-read
            self._drop_pad()

    def _read_direction(self) -> tuple[int, int]:
        """Merge the d-pad hat and the left stick into one direction."""
        dx = dy = 0
        pad = self._pad

        if pad.get_numhats() > 0:
            hx, hy = pad.get_hat(0)
            dx, dy = hx, -hy  # SDL hats are y-up; screen rows are y-down

        if dx == 0 and pad.get_numaxes() > 0:
            ax = pad.get_axis(0)
            if abs(ax) >= AXIS_DEADZONE:
                dx = 1 if ax > 0 else -1
        if dy == 0 and pad.get_numaxes() > 1:
            ay = pad.get_axis(1)
            if abs(ay) >= AXIS_DEADZONE:
                dy = 1 if ay > 0 else -1

        # One axis at a time — diagonal drift shouldn't change panel and row
        # in the same frame.
        if dx and dy:
            dy = 0
        return dx, dy

    def _pump_direction(self, dx: int, dy: int) -> None:
        if (dx, dy) != self._dir:
            self._dir = (dx, dy)
            self._held_ms = 0
            self._fired_ms = 0
            if dx or dy:
                self.moved.emit(dx, dy)
            return

        if not (dx or dy):
            return

        self._held_ms += POLL_MS
        if self._held_ms < REPEAT_DELAY_MS:
            return
        if self._held_ms - self._fired_ms >= REPEAT_RATE_MS:
            self._fired_ms = self._held_ms
            self.moved.emit(dx, dy)

    def _pump_buttons(self) -> None:
        pad = self._pad
        for index, signal in (
            (BTN_ACCEPT, self.accepted),
            (BTN_START, self.accepted),
            (BTN_BACK, self.cancelled),
        ):
            if index >= pad.get_numbuttons():
                continue
            down = bool(pad.get_button(index))
            if down and not self._buttons.get(index, False):
                signal.emit()
            self._buttons[index] = down
