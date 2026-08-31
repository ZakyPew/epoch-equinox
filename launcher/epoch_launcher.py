#!/usr/bin/env python3
"""Epoch & Equinox launcher.

A standalone launcher for the recompiled Oracle of Ages / Oracle of Seasons
runner, in the shape Zelda64Recomp and Ship of Harkinian use: the launcher is
its own process, and the game binary only runs carts.

That split is deliberate: UI iterates without touching the player, and a
crash in the game process can't take the launcher down with it.

The game table is not duplicated here — it comes from `epoch --games-json`,
so the two stay in sync from one source.

    python3 launcher/epoch_launcher.py [--runner path/to/oracles]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# PyInstaller extracts Python modules to a temporary directory. The player,
# ROMs, mods, and covers remain beside the launcher executable.
FROZEN = bool(getattr(sys, "frozen", False))
PROJECT_ROOT = (
    Path(sys.executable).resolve().parent
    if FROZEN
    else Path(__file__).resolve().parent.parent
)

BOOTSTRAP_HELP = """\
The Epoch & Equinox launcher needs PySide6.

    pip install -r launcher/requirements.txt

(or: pip install PySide6-Essentials, plus pygame for controller support)
"""

try:
    from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, Signal
except ImportError:
    # Reaching the user matters more than tidiness here: someone who
    # double-clicked this file has no terminal to read a traceback in.
    sys.stderr.write(BOOTSTRAP_HELP)
    try:
        import tkinter
        from tkinter import messagebox

        root = tkinter.Tk()
        root.withdraw()
        messagebox.showerror("Epoch & Equinox launcher - missing dependency", BOOTSTRAP_HELP)
    except Exception:
        pass
    raise SystemExit(1)

from PySide6.QtGui import (
    QBrush,
    QColor,
    QImage,
    QFont,
    QFontMetricsF,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
    QPolygonF,
    QRadialGradient,
)
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QGraphicsOpacityEffect,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QComboBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QLineEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

import oracle_secrets
import save_manager
import stream_config
import updater
from gamepad import GamepadBridge

# The version this build was released as. The updater compares it against
# the repository's release tags, so it has to match the tag it ships under
# -- the release workflow refuses to publish a tag that disagrees with it.
APP_VERSION = "0.7.0"

# The diagonal that splits the two panels. x is a fraction of the window
# width; the seam runs from (TOP, 0) down to (BOTTOM, height).
SEAM_TOP = 0.62
SEAM_BOTTOM = 0.38

# Cover art lookup. The cart ids are what the runner reports, but nobody
# drops a file called "tlozooa.png" into a folder without being told to, so
# each cart also answers to a plain-English name.
COVER_ALIASES = {
    "tlozooa": ["ages", "oracle-of-ages", "oracle_of_ages"],
    "tlozoos": ["seasons", "oracle-of-seasons", "oracle_of_seasons"],
}
COVER_EXTENSIONS = [".png", ".jpg", ".jpeg", ".webp"]


# --------------------------------------------------------------------------
# theme
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Theme:
    """Per-cart palette. Ages runs cool and tidal for the Harp; Seasons warm
    and autumnal for the Rod."""

    base: QColor
    glow: QColor
    accent: QColor
    ink: QColor

    @staticmethod
    def for_game(game_id: str) -> "Theme":
        if game_id == "tlozoos":
            return Theme(
                base=QColor(26, 10, 6),
                glow=QColor(150, 62, 20),
                accent=QColor(240, 178, 96),
                ink=QColor(246, 232, 214),
            )
        return Theme(
            base=QColor(6, 13, 26),
            glow=QColor(22, 74, 96),
            accent=QColor(138, 214, 208),
            ink=QColor(226, 240, 244),
        )


@dataclass
class Game:
    id: str
    title: str
    rom_path: str
    rom_size: int
    sha256: str
    rom_present: bool
    ready: bool
    mods: list = field(default_factory=list)

    @property
    def playable(self) -> bool:
        return self.rom_present or self.ready


# --------------------------------------------------------------------------
# runner / filesystem glue
# --------------------------------------------------------------------------


class Runner:
    """Everything that talks to the game binary or its data directories."""

    def __init__(self, exe: Path):
        self.exe = exe.resolve()
        self.root = self.exe.parent

    def query_games(self) -> list[Game]:
        out = subprocess.run(
            [str(self.exe), "--games-json"],
            capture_output=True,
            text=True,
            cwd=self.root,
            timeout=30,
        )
        if out.returncode != 0:
            raise RuntimeError(out.stderr.strip() or "runner failed")
        # The runner logs to stderr, so stdout is clean JSON — but be lenient
        # and pick out the object in case something else prints.
        text = out.stdout.strip()
        start = text.find("{")
        if start < 0:
            raise RuntimeError(f"unexpected --games-json output: {text[:200]!r}")
        data = json.loads(text[start:])
        return [Game(**g) for g in data["games"]]

    # -- mods ------------------------------------------------------------

    @property
    def mods_dir(self) -> Path:
        return self.root / "mods"

    @property
    def state_path(self) -> Path:
        return self.mods_dir / "state.json"

    def scan_mods(self, game_id: str) -> list[dict]:
        """Mirror of gb_mods_scan(): every mods/*/manifest.json whose "games"
        list covers this cart (a manifest with no "games" key covers all)."""
        found: list[dict] = []
        if not self.mods_dir.is_dir():
            return found
        state = self.read_state()
        for child in sorted(self.mods_dir.iterdir()):
            manifest = child / "manifest.json"
            if not manifest.is_file():
                continue
            try:
                m = json.loads(manifest.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                print(f"[launcher] skipping {manifest}: {exc}", file=sys.stderr)
                continue
            games = m.get("games")
            if games and game_id not in games:
                continue
            mod_id = m.get("id") or child.name
            found.append(
                {
                    "id": mod_id,
                    "name": m.get("name", mod_id),
                    "version": m.get("version", "0.0.0"),
                    "patch": m.get("patch", ""),
                    "priority": m.get("priority", 100),
                    "enabled": state.get(mod_id, m.get("enabled", True)),
                    "description": m.get("description", ""),
                    "author": m.get("author", ""),
                    "dir": child,
                }
            )
        found.sort(key=lambda m: (m["priority"], m["id"]))
        return found

    def read_state(self) -> dict:
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {}

    def write_state(self, updates: dict) -> None:
        state = self.read_state()
        state.update(updates)
        self.mods_dir.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(state, indent=2), encoding="utf-8")

    def install_rom(self, game: Game, source: Path) -> None:
        dest = self.root / game.rom_path
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)

    @property
    def run_log(self) -> Path:
        return self.root / "epoch-run.log"

    @staticmethod
    def _game_env() -> dict:
        """Environment for the game process. The gamepad bridge points SDL
        at its headless 'dummy' drivers so pygame can poll pads without a
        window -- but those variables are set process-wide and a spawned
        game inherits them, at which point SDL_CreateWindow fails with
        'OpenGL support is either not configured in SDL or not available
        in current SDL video driver (dummy)'. Strip them so the game gets
        real video and audio."""
        env = os.environ.copy()
        env.pop("SDL_VIDEODRIVER", None)
        env.pop("SDL_AUDIODRIVER", None)
        return env

    def launch(self, game: Game) -> subprocess.Popen:
        # The runner's output goes to a log file, not a pipe: a pipe nobody
        # drains would eventually block the game, and when the launcher is a
        # frozen windowed exe the inherited standard handles are invalid
        # anyway. The log also turns "the game just didn't start" into a
        # readable error -- start_game checks for an early exit and shows
        # the tail of this file.
        log = open(self.run_log, "w", encoding="utf-8", errors="replace")
        kwargs = {}
        if os.name == "nt":
            # Don't flash a console window behind the game.
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            return subprocess.Popen(
                [str(self.exe), "--game", game.id],
                cwd=self.root,
                env=self._game_env(),
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                **kwargs,
            )
        finally:
            # The child holds its own duplicate of the handle.
            log.close()


# --------------------------------------------------------------------------
# painting helpers
# --------------------------------------------------------------------------


def title_font(size: float, weight: QFont.Weight = QFont.Weight.Normal) -> QFont:
    f = QFont()
    f.setFamilies(["Georgia", "Palatino", "Times New Roman", "DejaVu Serif", "Serif"])
    f.setPointSizeF(size)
    f.setWeight(weight)
    return f


def label_font(size: float, spacing: float = 3.0) -> QFont:
    f = QFont()
    f.setFamilies(["Inter", "Segoe UI", "DejaVu Sans", "Sans Serif"])
    f.setPointSizeF(size)
    f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, spacing)
    return f


def seam_x(width: float, y: float, height: float) -> float:
    t = y / height if height else 0.0
    return width * (SEAM_TOP + (SEAM_BOTTOM - SEAM_TOP) * t)


def panel_path(index: int, w: float, h: float) -> QPainterPath:
    """Left (0) or right (1) side of the diagonal seam."""
    p = QPainterPath()
    if index == 0:
        p.moveTo(0, 0)
        p.lineTo(seam_x(w, 0, h), 0)
        p.lineTo(seam_x(w, h, h), h)
        p.lineTo(0, h)
    else:
        p.moveTo(seam_x(w, 0, h), 0)
        p.lineTo(w, 0)
        p.lineTo(w, h)
        p.lineTo(seam_x(w, h, h), h)
    p.closeSubpath()
    return p


# The background motif is meant to be felt, not read - the reference
# launchers keep theirs at a few percent opacity so the title stays the only
# thing with real contrast. Everything here is deliberately near-invisible.
MOTIF_GAIN = 1.0


def draw_ages_motif(pr: QPainter, c: QPointF, r: float, col: QColor) -> None:
    """Concentric tidal rings behind a tall hourglass, for the Harp of Ages."""
    pr.save()
    pr.setBrush(Qt.BrushStyle.NoBrush)
    for i in range(8, 0, -1):
        ring = QColor(col)
        ring.setAlphaF(min(1.0, (0.008 + 0.0045 * (9 - i)) * MOTIF_GAIN))
        pr.setPen(QPen(ring, max(1.0, r * 0.007)))
        rad = r * (0.13 + 0.107 * i)
        pr.drawEllipse(c, rad, rad)

    glass = QColor(col)
    glass.setAlphaF(min(1.0, 0.030 * MOTIF_GAIN))
    hw, hh = r * 0.30, r * 0.46
    pr.setPen(Qt.PenStyle.NoPen)
    pr.setBrush(QBrush(glass))
    pr.drawPolygon(
        QPolygonF([QPointF(c.x() - hw, c.y() - hh), QPointF(c.x() + hw, c.y() - hh), c])
    )
    pr.drawPolygon(
        QPolygonF([QPointF(c.x() - hw, c.y() + hh), QPointF(c.x() + hw, c.y() + hh), c])
    )
    pr.restore()


def draw_seasons_motif(pr: QPainter, c: QPointF, r: float, col: QColor) -> None:
    """The Rod's four-season wheel."""
    pr.save()
    pr.setPen(Qt.PenStyle.NoPen)
    wedges = [
        QColor(120, 180, 96),   # spring
        QColor(236, 198, 96),   # summer
        QColor(204, 100, 42),   # autumn
        QColor(206, 228, 240),  # winter
    ]
    rad = r * 0.68
    box = QRectF(c.x() - rad, c.y() - rad, rad * 2, rad * 2)
    for i, wc in enumerate(wedges):
        tint = QColor(wc)
        tint.setAlphaF(min(1.0, 0.028 * MOTIF_GAIN))
        pr.setBrush(QBrush(tint))
        # Qt angles are 1/16 degree, counter-clockwise from 3 o'clock.
        pr.drawPie(box, int(i * 90 * 16), int(90 * 16))

    ring = QColor(col)
    ring.setAlphaF(min(1.0, 0.045 * MOTIF_GAIN))
    pr.setPen(QPen(ring, max(1.2, r * 0.008)))
    pr.setBrush(Qt.BrushStyle.NoBrush)
    pr.drawEllipse(c, rad, rad)
    pr.drawEllipse(c, rad * 0.62, rad * 0.62)

    spoke = QColor(col)
    spoke.setAlphaF(min(1.0, 0.035 * MOTIF_GAIN))
    pr.setPen(QPen(spoke, max(1.0, r * 0.006)))
    for i in range(4):
        a = math.radians(i * 90 + 45)
        pr.drawLine(c, QPointF(c.x() + math.cos(a) * rad, c.y() - math.sin(a) * rad))
    pr.restore()


# --------------------------------------------------------------------------
# main view
# --------------------------------------------------------------------------

MENU_ITEMS = [
    "Start game", "Continue Legend", "Mods", "Achievements", "Secrets",
    "Saves", "Install ROM", "Stream", "Updates", "Exit",
]

# The two halves of the legend. Continuing one means linking the other.
GAME_PAIR = {"tlozooa": "tlozoos", "tlozoos": "tlozooa"}


class LauncherView(QWidget):
    action = Signal(str, object)  # (menu item, Game)

    def __init__(self, runner: Runner, games: list[Game]):
        super().__init__()
        self.runner = runner
        self.games = games
        self.active = 0
        self.menu_index = 0
        self.pad_name = ""
        self.update_note = ""
        self._menu_rects: list[QRectF] = []
        self._covers: dict[str, QPixmap | None] = {}
        self.setMouseTracking(True)
        self.setMinimumSize(900, 520)

    # -- data ------------------------------------------------------------

    def refresh(self, games: list[Game]) -> None:
        self.games = games
        self.active = min(self.active, max(0, len(games) - 1))
        self.update()

    def active_game(self) -> Game | None:
        return self.games[self.active] if self.games else None

    def cover(self, game_id: str) -> QPixmap | None:
        """Panel art for a cart, most specific source first:

        1. ``covers/<id>.png`` next to the binary -- one machine's own art,
           gitignored, so scans and key art stay off the repo
        2. ``art/covers/<id>.png`` in the project -- art shipped with it
        3. neither: the caller falls back to the procedural motif

        Friendly aliases (``ages.png`` / ``seasons.png``) are accepted
        alongside the cart ids, because ``tlozooa`` is not a name anybody
        would guess when dragging a file into GitHub's upload box. Several
        extensions are tried for the same reason.
        """
        if game_id in self._covers:
            return self._covers[game_id]

        stems = [game_id] + COVER_ALIASES.get(game_id, [])
        dirs = [self.runner.root / "covers", PROJECT_ROOT / "art" / "covers"]

        chosen = None
        for directory in dirs:
            for stem in stems:
                for ext in COVER_EXTENSIONS:
                    path = directory / f"{stem}{ext}"
                    if not path.is_file():
                        continue
                    pm = QPixmap(str(path))
                    if not pm.isNull():
                        chosen = pm
                        break
                    print(f"[launcher] could not decode {path}", file=sys.stderr)
                if chosen:
                    break
            if chosen:
                break

        self._covers[game_id] = chosen
        return chosen

    # -- input -----------------------------------------------------------

    def _panel_at(self, x: float, y: float) -> int:
        return 0 if x < seam_x(self.width(), y, self.height()) else 1

    def navigate(self, dx: int, dy: int) -> None:
        """Abstract cursor movement, shared by the arrow keys and the pad."""
        if dx and len(self.games) > 1:
            self.active = max(0, min(len(self.games) - 1, self.active + dx))
        if dy:
            self.menu_index = (self.menu_index + dy) % len(MENU_ITEMS)
        self.update()

    def activate(self) -> None:
        self.action.emit(MENU_ITEMS[self.menu_index], self.active_game())

    def set_pad_name(self, name: str) -> None:
        self.pad_name = name
        self.update()

    def set_update_note(self, note: str) -> None:
        self.update_note = note
        self.update()

    def mouseMoveEvent(self, event) -> None:
        pos = event.position()
        idx = self._panel_at(pos.x(), pos.y())
        changed = False
        if idx != self.active and idx < len(self.games):
            self.active = idx
            changed = True

        over_menu = False
        for i, r in enumerate(self._menu_rects):
            if r.contains(pos):
                over_menu = True
                if i != self.menu_index:
                    self.menu_index = i
                    changed = True
                break
        self.setCursor(
            Qt.CursorShape.PointingHandCursor if over_menu else Qt.CursorShape.ArrowCursor
        )
        if changed:
            self.update()

    def mousePressEvent(self, event) -> None:
        pos = event.position()
        for i, r in enumerate(self._menu_rects):
            if r.contains(pos):
                self.action.emit(MENU_ITEMS[i], self.active_game())
                return
        idx = self._panel_at(pos.x(), pos.y())
        if idx < len(self.games) and idx != self.active:
            self.active = idx
            self.update()

    def keyPressEvent(self, event) -> None:
        key = event.key()
        if key in (Qt.Key.Key_Left, Qt.Key.Key_A):
            self.navigate(-1, 0)
        elif key in (Qt.Key.Key_Right, Qt.Key.Key_D):
            self.navigate(1, 0)
        elif key in (Qt.Key.Key_Up, Qt.Key.Key_W):
            self.navigate(0, -1)
        elif key in (Qt.Key.Key_Down, Qt.Key.Key_S):
            self.navigate(0, 1)
        elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space):
            self.activate()
        elif key == Qt.Key.Key_Escape:
            self.action.emit("Exit", self.active_game())
        else:
            super().keyPressEvent(event)

    # -- paint -----------------------------------------------------------

    def paintEvent(self, _event) -> None:
        pr = QPainter(self)
        pr.setRenderHints(
            QPainter.RenderHint.Antialiasing | QPainter.RenderHint.TextAntialiasing
        )
        w, h = float(self.width()), float(self.height())
        pr.fillRect(self.rect(), QColor(8, 8, 10))

        self._menu_rects = []
        for i, game in enumerate(self.games[:2]):
            self._paint_panel(pr, i, game, w, h, active=(i == self.active))

        # Seam: a soft light line rather than a hard edge.
        seam = QPainterPath()
        seam.moveTo(seam_x(w, 0, h), 0)
        seam.lineTo(seam_x(w, h, h), h)
        pr.setPen(QPen(QColor(255, 255, 255, 26), 1.4))
        pr.drawPath(seam)

        pr.setFont(label_font(7.5, 1.6))
        pr.setPen(QColor(150, 150, 158))
        footer = f"v{APP_VERSION}"
        if self.pad_name:
            footer += f"   ·   {self.pad_name}"
        if self.update_note:
            footer += f"   ·   {self.update_note}"
        pr.drawText(QRectF(18, h - 30, w * 0.6, 20), Qt.AlignmentFlag.AlignVCenter, footer)

    def _paint_panel(
        self, pr: QPainter, index: int, game: Game, w: float, h: float, active: bool
    ) -> None:
        theme = Theme.for_game(game.id)
        path = panel_path(index, w, h)

        pr.save()
        pr.setClipPath(path)

        # Base wash, brighter on the active side so the seam reads as depth.
        grad = QLinearGradient(0, 0, 0, h)
        base = QColor(theme.base)
        if not active:
            base = base.darker(150)
        grad.setColorAt(0.0, base.lighter(112))
        grad.setColorAt(1.0, base)
        pr.fillPath(path, QBrush(grad))

        cx = w * (0.30 if index == 0 else 0.72)
        centre = QPointF(cx, h * 0.52)
        radius = min(w, h) * 0.80

        halo = QRadialGradient(centre, radius * 1.5)
        g = QColor(theme.glow)
        g.setAlphaF(0.55 if active else 0.22)
        halo.setColorAt(0.0, g)
        halo.setColorAt(1.0, QColor(0, 0, 0, 0))
        pr.fillPath(path, QBrush(halo))

        cover = self.cover(game.id)
        if cover is not None:
            # Scale to cover the *panel*, not the whole window. The panel is
            # the widest slice the diagonal leaves (about 0.62 of the width),
            # so its aspect is roughly 1.1:1 -- which is why the art guidance
            # in covers/README.md asks for a square image.
            panel_w = w * max(SEAM_TOP, 1.0 - SEAM_BOTTOM)
            scaled = cover.scaled(
                int(panel_w),
                int(h),
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation,
            )
            # Both covers stay legible. Drawing the inactive one at a
            # quarter opacity over a near-black ground assumed bright art;
            # a dark cover just turned to mud. The active/inactive
            # hierarchy comes from the veil below and the text treatment,
            # not from crushing the artwork.
            pr.setOpacity(1.0 if active else 0.72)
            pr.drawPixmap(
                int(cx - scaled.width() / 2), int(h / 2 - scaled.height() / 2), scaled
            )
            pr.setOpacity(1.0)

            if not active:
                pr.fillPath(path, QBrush(QColor(6, 8, 12, 92)))

            # Scrim under the text side. Custom art is drawn bright enough to
            # actually look at, so the title and menu need their own contrast
            # rather than relying on the art being dim.
            # Only as much scrim as the text actually needs. Fading to
            # clear at 0.55 shaded over half the artwork, which on a dark
            # cover read as "the whole panel is dim".
            scrim = QLinearGradient(0, 0, w, 0)
            dark = QColor(0, 0, 0, 188)
            clear = QColor(0, 0, 0, 0)
            if index == 0:
                scrim.setColorAt(0.0, dark)
                scrim.setColorAt(0.34, clear)
            else:
                scrim.setColorAt(0.66, clear)
                scrim.setColorAt(1.0, dark)
            pr.fillPath(path, QBrush(scrim))
        else:
            pr.setOpacity(1.0 if active else 0.45)
            motif = QColor(theme.accent)
            if game.id == "tlozoos":
                draw_seasons_motif(pr, centre, radius, motif)
            else:
                draw_ages_motif(pr, centre, radius, motif)
            pr.setOpacity(1.0)

        self._paint_text(pr, index, game, theme, w, h, active)
        pr.restore()

    @staticmethod
    def _text(pr: QPainter, rect: QRectF, align, s: str, ink: QColor,
              shadow: float = 1.6) -> None:
        """Draw text with a soft dark offset behind it.

        The scrim is deliberately narrow now, so text can land on bright
        artwork (snowfields, waterfalls). A shadow costs nothing and keeps
        every label readable regardless of what cover art someone drops in.
        """
        if shadow > 0.0:
            dark = QColor(0, 0, 0, 165)
            dark.setAlphaF(dark.alphaF() * ink.alphaF())
            pr.setPen(dark)
            pr.drawText(rect.translated(shadow, shadow), align, s)
        pr.setPen(ink)
        pr.drawText(rect, align, s)

    def _paint_text(
        self,
        pr: QPainter,
        index: int,
        game: Game,
        theme: Theme,
        w: float,
        h: float,
        active: bool,
    ) -> None:
        left = index == 0
        margin = w * 0.045
        align = Qt.AlignmentFlag.AlignLeft if left else Qt.AlignmentFlag.AlignRight
        box_w = w * 0.42
        x = margin if left else w - margin - box_w

        ink = QColor(theme.ink)
        if not active:
            ink.setAlphaF(0.42)

        pr.setFont(label_font(8.5, 3.2))
        sub = QColor(theme.accent)
        sub.setAlphaF(0.85 if active else 0.35)
        self._text(pr, QRectF(x, h * 0.13, box_w, 22), align,
                   "EPOCH & EQUINOX", sub, 1.2)

        pr.setFont(title_font(30 if active else 27, QFont.Weight.Light))
        self._text(pr, QRectF(x, h * 0.16, box_w, 64), align, game.title, ink, 2.0)

        if not game.playable:
            warn = QColor(236, 150, 140)
            warn.setAlphaF(0.95 if active else 0.4)
            pr.setFont(label_font(8.0, 2.6))
            self._text(pr, QRectF(x, h * 0.245, box_w, 20), align,
                       "ROM REQUIRED", warn, 1.2)

        if active:
            self._paint_menu(pr, game, theme, x, box_w, h, align)

    def _paint_menu(
        self,
        pr: QPainter,
        game: Game,
        theme: Theme,
        x: float,
        box_w: float,
        h: float,
        align: Qt.AlignmentFlag,
    ) -> None:
        font = title_font(15.5, QFont.Weight.Normal)
        pr.setFont(font)
        fm = QFontMetricsF(font)
        line_h = fm.height() * 1.85
        top = h - 40 - line_h * len(MENU_ITEMS)

        for i, item in enumerate(MENU_ITEMS):
            rect = QRectF(x, top + i * line_h, box_w, line_h)
            self._menu_rects.append(rect)

            disabled = item in ("Start game", "Mods") and not game.playable
            if disabled:
                col = QColor(150, 150, 155, 90)
            elif i == self.menu_index:
                col = QColor(theme.accent)
            else:
                col = QColor(theme.ink)
                col.setAlphaF(0.86)
            self._text(pr, rect, align | Qt.AlignmentFlag.AlignVCenter,
                       f"{item}  ·", col, 1.8)


# --------------------------------------------------------------------------
# mods dialog
# --------------------------------------------------------------------------


# --------------------------------------------------------------------------
# secrets dialog
# --------------------------------------------------------------------------


def load_icon_pixmap(folder: Path, achievement_id: str) -> QPixmap:
    """An achievement's icon as a pixmap, PAM first then legacy PPM.

    Qt reads PPM but not PAM, and the icons carry real alpha now, so the
    PAM is decoded here: a short text header, then RGBA bytes. Falls back
    to the chroma-keyed PPM a mod might still ship, turning its magenta
    into transparency the way the player does.
    """
    pam = folder / f"{achievement_id}.pam"
    if pam.is_file():
        try:
            data = pam.read_bytes()
            head, _, body = data.partition(b"ENDHDR\n")
            fields = dict(
                line.split(maxsplit=1)  # WIDTH 48 -> ("WIDTH", "48")
                for line in (l.strip().decode("ascii", "replace")
                             for l in head.splitlines()[1:])
                if line and " " in line
            )
            w, h = int(fields["WIDTH"]), int(fields["HEIGHT"])
            if int(fields.get("DEPTH", 4)) == 4 and len(body) >= w * h * 4:
                img = QImage(body[: w * h * 4], w, h, w * 4,
                             QImage.Format.Format_RGBA8888)
                # QImage does not copy the buffer it is handed.
                return QPixmap.fromImage(img.copy())
        except (OSError, KeyError, ValueError):
            pass

    ppm = folder / f"{achievement_id}.ppm"
    pm = QPixmap(str(ppm))
    if pm.isNull():
        return pm
    img = pm.toImage().convertToFormat(QImage.Format.Format_ARGB32)
    img.setAlphaChannel(img.createMaskFromColor(0xFFFF00FF,
                                                Qt.MaskMode.MaskOutColor))
    return QPixmap.fromImage(img)


class SecretsDialog(QDialog):
    """Every code this save can produce, spelled in the game's symbols.

    Oracle secrets are per-file: each save carries a random Game ID and
    every code validates against it, so codes from a website will not
    work. These are generated from the save itself — the same codes the
    NPCs would speak.
    """

    def __init__(self, runner: Runner, game: Game, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"Secrets - {game.title}")
        self.setMinimumSize(600, 520)
        self.setStyleSheet(
            """
            QDialog { background: #12151a; }
            QLabel { color: #d6dde4; }
            QPushButton {
                background: #23303a; color: #dfe8ee; border: 0; padding: 8px 18px;
                border-radius: 6px;
            }
            QPushButton:hover { background: #2e414f; }
            """
        )

        layout = QVBoxLayout(self)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet("QScrollArea { border: 0; background: #12151a; }")
        inner = QWidget()
        inner.setStyleSheet("background: #12151a;")
        rows = QVBoxLayout(inner)

        sav = oracle_secrets.find_saves(runner.root).get(game.id)
        saves = oracle_secrets.read_save(sav) if sav else []

        if not saves:
            empty = QLabel(
                "No save file found for this game yet. Play once and save, "
                "then come back — secrets are made from your own file."
            )
            empty.setWordWrap(True)
            rows.addWidget(empty)
        for save in saves:
            rows.addWidget(self._save_header(save))
            other = "Seasons" if save.game == "ages" else "Ages"
            kind = "hero's secret" if (save.is_linked or save.is_hero) \
                else "linked-game secret"
            self._add_secret(
                rows, f"Game secret — start a new game in {other} ({kind})",
                oracle_secrets.game_secret(save))
            self._add_secret(
                rows,
                f"Ring secret — carries your {oracle_secrets.ring_count(save)}"
                f" ring(s) to the linked game (tell it to Red Snake in Vasu's)",
                oracle_secrets.ring_secret(save))

            for title, table in (
                ("Secrets to tell in Oracle of Ages",
                 oracle_secrets.AGES_NPC_SECRETS),
                ("Secrets to tell in Oracle of Seasons",
                 oracle_secrets.SEASONS_NPC_SECRETS),
            ):
                head = QLabel(title)
                head.setStyleSheet(
                    "font-size: 13px; font-weight: bold; color: #d0aa55; "
                    "margin-top: 10px;"
                )
                rows.addWidget(head)
                note = QLabel(
                    "Each NPC answers with a return secret for Farore; "
                    "it is shown dimmed in case you lose it."
                )
                note.setWordWrap(True)
                note.setStyleSheet("color: #6b7680; font-size: 10px;")
                rows.addWidget(note)
                for index, npc in table.items():
                    code = oracle_secrets.to_text(
                        oracle_secrets.short_secret(save, index))
                    ret = oracle_secrets.to_text(
                        oracle_secrets.short_secret(
                            save, index + oracle_secrets.RETURN_OFFSET))
                    row = QHBoxLayout()
                    name = QLabel(npc)
                    name.setFixedWidth(130)
                    name.setStyleSheet("color: #a8b3bd; font-size: 12px;")
                    row.addWidget(name)
                    row.addWidget(self._code_label(code))
                    ret_label = self._code_label(ret)
                    ret_label.setStyleSheet(
                        ret_label.styleSheet() + " color: #566068;")
                    row.addWidget(ret_label)
                    row.addStretch(1)
                    rows.addLayout(row)

        rows.addStretch(1)
        scroll.setWidget(inner)
        layout.addWidget(scroll, 1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _save_header(self, save) -> QLabel:
        badges = []
        if save.is_hero:
            badges.append("hero's game")
        elif save.is_linked:
            badges.append("linked game")
        if save.game_id:
            badges.append(f"Game ID {save.game_id:04X}")
        else:
            badges.append(
                "no Game ID yet — the game assigns one on first secret use; "
                "codes made now are accepted by any file"
            )
        head = QLabel(
            f"File {save.slot + 1}:  {save.hero_name}   ·   "
            + "   ·   ".join(badges)
        )
        head.setWordWrap(True)
        head.setStyleSheet(
            "font-size: 14px; font-weight: bold; margin-top: 6px;"
        )
        return head

    def _add_secret(self, rows, caption: str, cells: list[int]) -> None:
        label = QLabel(caption)
        label.setWordWrap(True)
        label.setStyleSheet("color: #a8b3bd; font-size: 12px; margin-top: 8px;")
        rows.addWidget(label)
        rows.addWidget(self._code_label(oracle_secrets.to_text(cells)))

    def _code_label(self, text: str) -> QLabel:
        code = QLabel(text)
        code.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse)
        code.setStyleSheet(
            "font-family: monospace; font-size: 15px; color: #f2ecda; "
            "background: #1a2030; border-radius: 4px; padding: 4px 8px;"
        )
        return code


# --------------------------------------------------------------------------
# saves dialog
# --------------------------------------------------------------------------


def open_in_file_manager(d: Path) -> None:
    """Open a folder in the system file manager, creating it first so the
    window that appears is a usable drop target rather than an error."""
    d.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        os.startfile(str(d))  # noqa: S606 - opening our own folder
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(d)])
    else:
        subprocess.Popen(["xdg-open", str(d)])


class SavesDialog(QDialog):
    """Import, export and back up this game's battery save.

    The rule the whole dialog obeys: nothing here loses a file. Anything
    that replaces the save -- an import, a restore -- backs the old one
    up first, and the backups are listed right below with their own
    Restore buttons, so every step is one click to undo.
    """

    def __init__(self, runner: Runner, game: Game, parent=None):
        super().__init__(parent)
        self.runner = runner
        self.game = game
        self.setWindowTitle(f"Saves - {game.title}")
        self.setMinimumSize(560, 460)
        self.setStyleSheet(DIALOG_STYLE)

        layout = QVBoxLayout(self)
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setStyleSheet("QScrollArea { border: 0; }")
        layout.addWidget(self.scroll, 1)

        note = QLabel(
            "Anything that replaces the save backs the old one up first "
            "(save-backups/, next to the game). Swap saves while the game "
            "is closed — it only reads the file at launch and overwrites "
            "it when you save in game."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: #8b97a2; font-size: 11px;")
        layout.addWidget(note)

        row = QHBoxLayout()
        imp = QPushButton("Import Save…")
        imp.clicked.connect(self.import_save)
        exp = QPushButton("Export a Copy…")
        exp.clicked.connect(self.export_save)
        back = QPushButton("Back Up Now")
        back.clicked.connect(self.backup_now)
        folder = QPushButton("Open Save Folder")
        folder.clicked.connect(
            lambda: open_in_file_manager(self.runner.root))
        for b in (imp, exp, back, folder):
            row.addWidget(b)
        row.addStretch(1)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        row.addWidget(buttons)
        layout.addLayout(row)

        self.rebuild()

    # -- state ------------------------------------------------------------

    def save_path(self) -> Path | None:
        return save_manager.current_save(
            self.runner.root, self.game.id, self.game.rom_path)

    def rebuild(self) -> None:
        inner = QWidget()
        rows = QVBoxLayout(inner)

        sav = self.save_path()
        if sav is None:
            head = QLabel(
                "No save file yet. Play once and save in game, or import "
                "one below — a save from any emulator works, it is the "
                "same battery file."
            )
            head.setWordWrap(True)
            rows.addWidget(head)
        else:
            st = sav.stat()
            when = datetime.fromtimestamp(st.st_mtime).strftime(
                "%Y-%m-%d %H:%M")
            head = QLabel(f"<b>{sav.name}</b>   ·   {st.st_size:,} bytes"
                          f"   ·   saved {when}")
            head.setTextFormat(Qt.TextFormat.RichText)
            rows.addWidget(head)
            for save in oracle_secrets.read_save(sav):
                badges = []
                if save.is_hero:
                    badges.append("hero's game")
                elif save.is_linked:
                    badges.append("linked game")
                if save.game_id:
                    badges.append(f"Game ID {save.game_id:04X}")
                text = f"File {save.slot + 1}:  {save.hero_name}"
                if badges:
                    text += "   ·   " + "   ·   ".join(badges)
                lab = QLabel(text)
                lab.setStyleSheet("color: #a8b3bd; font-size: 12px; "
                                  "margin-left: 12px;")
                rows.addWidget(lab)
                stats = QLabel(
                    f"{save.essences}/8 essences   ·   "
                    f"{save.hearts} hearts   ·   "
                    f"{oracle_secrets.ring_count(save)} rings   ·   "
                    f"{save.deaths} deaths   ·   "
                    f"{save.playtime} played")
                stats.setStyleSheet("color: #6b7680; font-size: 11px; "
                                    "margin-left: 24px;")
                rows.addWidget(stats)

        backups = save_manager.list_backups(self.runner.root, self.game.id)
        if backups:
            head = QLabel("Backups")
            head.setStyleSheet("font-size: 13px; font-weight: bold; "
                               "color: #d0aa55; margin-top: 12px;")
            rows.addWidget(head)
        for bak in backups[:20]:
            row = QHBoxLayout()
            lab = QLabel(f"{bak.name}   ·   {bak.stat().st_size:,} bytes")
            lab.setStyleSheet("color: #a8b3bd; font-size: 12px;")
            row.addWidget(lab)
            row.addStretch(1)
            btn = QPushButton("Restore")
            btn.setStyleSheet("padding: 4px 12px; font-size: 11px;")
            btn.clicked.connect(
                lambda _=False, b=bak: self.restore(b))
            row.addWidget(btn)
            rows.addLayout(row)
        if len(backups) > 20:
            more = QLabel(f"…and {len(backups) - 20} older, in the folder.")
            more.setStyleSheet("color: #6b7680; font-size: 11px;")
            rows.addWidget(more)

        rows.addStretch(1)
        self.scroll.setWidget(inner)

    # -- operations -------------------------------------------------------

    def import_save(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, f"Import a {self.game.title} save", "",
            "Battery saves (*.sav *.srm);;All files (*)")
        if not path:
            return
        source = Path(path)
        try:
            verdict = save_manager.check_import(source, self.game.id)
        except save_manager.SaveError as exc:
            QMessageBox.warning(self, "Not imported", str(exc))
            return
        if verdict.empty:
            answer = QMessageBox.question(
                self, "No files inside",
                f"{source.name} has no recognizable file slots — it may "
                "be a fresh save, or not an Oracle save at all. Import "
                "it anyway?")
            if answer != QMessageBox.StandardButton.Yes:
                return
        elif self.save_path() is not None:
            names = ", ".join(
                s.hero_name for s in verdict.slots) or "no one"
            answer = QMessageBox.question(
                self, "Replace the current save?",
                f"{source.name} carries: {names}.\n\nYour current save "
                "is backed up first, so this is one click to undo.")
            if answer != QMessageBox.StandardButton.Yes:
                return
        try:
            save_manager.import_save(
                self.runner.root, self.game.id, self.game.rom_path, source)
        except (save_manager.SaveError, OSError) as exc:
            QMessageBox.warning(self, "Not imported", str(exc))
            return
        self.rebuild()

    def export_save(self) -> None:
        sav = self.save_path()
        if sav is None:
            QMessageBox.information(
                self, "Nothing to export", "There is no save file yet.")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Export a copy of the save",
            save_manager.export_name(self.game.id),
            "Battery saves (*.sav);;All files (*)")
        if not path:
            return
        try:
            save_manager.export_save(sav, Path(path))
        except OSError as exc:
            QMessageBox.warning(self, "Could not export", str(exc))

    def backup_now(self) -> None:
        sav = self.save_path()
        if sav is None:
            QMessageBox.information(
                self, "Nothing to back up", "There is no save file yet.")
            return
        try:
            save_manager.backup(self.runner.root, self.game.id, sav)
        except OSError as exc:
            QMessageBox.warning(self, "Could not back up", str(exc))
            return
        self.rebuild()

    def restore(self, bak: Path) -> None:
        answer = QMessageBox.question(
            self, "Restore this backup?",
            f"{bak.name} becomes the game's save. What it replaces is "
            "backed up first.")
        if answer != QMessageBox.StandardButton.Yes:
            return
        try:
            save_manager.restore(
                self.runner.root, self.game.id, self.game.rom_path, bak)
        except (save_manager.SaveError, OSError) as exc:
            QMessageBox.warning(self, "Not restored", str(exc))
            return
        self.rebuild()


# --------------------------------------------------------------------------
# achievements dialog
# --------------------------------------------------------------------------


def parse_achievement_packs(root: Path, cart: str) -> list[dict]:
    """Read achievements/<cart>.txt plus <cart>.*.txt, the same files the
    player loads. Returns [{id, title, desc}] in file order."""
    folder = root / "achievements"
    packs = [folder / f"{cart}.txt"]
    if folder.is_dir():
        packs += sorted(
            p for p in folder.glob(f"{cart}.*.txt") if p.name != f"{cart}.txt"
        )
    entries: list[dict] = []
    for pack in packs:
        try:
            text = pack.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        current: dict | None = None
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]") and len(line) > 2:
                current = {"id": line[1:-1], "title": "", "desc": ""}
                entries.append(current)
                continue
            if current is None or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            if key in ("title", "desc"):
                current[key] = value.strip()
    return entries


def read_unlocked(root: Path, cart: str) -> set[str]:
    try:
        text = (root / "states" / f"achievements-{cart}.txt").read_text(
            encoding="utf-8", errors="replace"
        )
    except OSError:
        return set()
    return {line.strip() for line in text.splitlines() if line.strip()}


class AchievementsDialog(QDialog):
    """The browser: everything the pack defines, earned entries lit."""

    def __init__(self, runner: Runner, game: Game, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"Achievements - {game.title}")
        self.setMinimumSize(560, 480)
        self.setStyleSheet(
            """
            QDialog { background: #12151a; }
            QLabel { color: #d6dde4; }
            QPushButton {
                background: #23303a; color: #dfe8ee; border: 0; padding: 8px 18px;
                border-radius: 6px;
            }
            QPushButton:hover { background: #2e414f; }
            """
        )

        entries = parse_achievement_packs(runner.root, game.id)
        unlocked = read_unlocked(runner.root, game.id)
        icon_dir = runner.root / "achievements" / "icons" / game.id

        layout = QVBoxLayout(self)
        tally = QLabel(
            f"{sum(1 for e in entries if e['id'] in unlocked)} of "
            f"{len(entries)} earned"
        )
        tally.setStyleSheet("font-size: 15px; font-weight: bold;")
        layout.addWidget(tally)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet(
            "QScrollArea { border: 0; background: #12151a; }"
        )
        inner = QWidget()
        inner.setStyleSheet("background: #12151a;")
        rows = QVBoxLayout(inner)

        for e in entries:
            earned = e["id"] in unlocked
            row = QHBoxLayout()

            icon = QLabel()
            icon.setFixedSize(48, 48)
            pm = load_icon_pixmap(icon_dir, e["id"])
            if pm.isNull():
                # No art yet: a plain medal dot keeps the rows aligned.
                icon.setText("\U0001F3C5")
                icon.setStyleSheet("font-size: 30px;")
                icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
            else:
                icon.setPixmap(
                    pm.scaled(48, 48, Qt.AspectRatioMode.KeepAspectRatio)
                )
            row.addWidget(icon)

            text = QVBoxLayout()
            title = QLabel(e["title"] or e["id"])
            desc = QLabel(e["desc"])
            desc.setWordWrap(True)
            if earned:
                # Explicit colors: a per-widget stylesheet stops the
                # dialog-level QLabel color from cascading in.
                title.setStyleSheet(
                    "font-size: 13px; font-weight: bold; color: #f2ecda;"
                )
                desc.setStyleSheet("color: #8b97a2; font-size: 11px;")
            else:
                title.setStyleSheet(
                    "font-size: 13px; font-weight: bold; color: #5a636d;"
                )
                desc.setStyleSheet("color: #4a525b; font-size: 11px;")
                effect = QGraphicsOpacityEffect(icon)
                effect.setOpacity(0.35)
                icon.setGraphicsEffect(effect)
            text.addWidget(title)
            text.addWidget(desc)
            row.addLayout(text, 1)
            rows.addLayout(row)
            rows.addSpacing(6)

        rows.addStretch(1)
        scroll.setWidget(inner)
        layout.addWidget(scroll, 1)

        note = QLabel(
            "Earned across every playthrough of this cart. Unlocks pop as a "
            "toast over the window during play; the list also lives in the "
            "in-game panel on F2."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: #8b97a2; font-size: 11px;")
        layout.addWidget(note)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)


class ModsDialog(QDialog):
    def __init__(self, runner: Runner, game: Game, parent=None):
        super().__init__(parent)
        self.runner = runner
        self.game = game
        self.boxes: list[tuple[str, QCheckBox]] = []

        self.setWindowTitle(f"Mods - {game.title}")
        self.setMinimumSize(520, 420)
        self.setStyleSheet(
            """
            QDialog { background: #12151a; }
            QLabel { color: #d6dde4; }
            QCheckBox { color: #d6dde4; padding: 6px; font-size: 13px; }
            QCheckBox::indicator { width: 16px; height: 16px; }
            QPushButton {
                background: #23303a; color: #dfe8ee; border: 0; padding: 8px 18px;
                border-radius: 6px;
            }
            QPushButton:hover { background: #2e414f; }
            """
        )

        layout = QVBoxLayout(self)

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setStyleSheet("QScrollArea { border: 0; }")
        layout.addWidget(self.scroll, 1)

        note = QLabel(
            "Changes apply on the next launch. The stock ROM is kept as a "
            "pristine snapshot, so turning a mod off fully undoes it."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: #8b97a2; font-size: 11px;")
        layout.addWidget(note)

        row = QHBoxLayout()
        open_btn = QPushButton("Open Mods Folder")
        open_btn.clicked.connect(self.open_mods_folder)
        refresh_btn = QPushButton("Refresh")
        refresh_btn.clicked.connect(self.rebuild)
        row.addWidget(open_btn)
        row.addWidget(refresh_btn)
        row.addStretch(1)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        row.addWidget(buttons)
        layout.addLayout(row)

        self.rebuild()

    def open_mods_folder(self) -> None:
        open_in_file_manager(self.runner.mods_dir)

    def rebuild(self) -> None:
        """(Re)scan mods/ and rebuild the list. Wired to Refresh so 'drop
        folder in, click Refresh' works without reopening the dialog."""
        self.boxes = []
        inner = QWidget()
        box_layout = QVBoxLayout(inner)
        mods = self.runner.scan_mods(self.game.id)

        if not mods:
            hint = QLabel(
                f"Nothing in <code>mods/</code> applies to <b>{self.game.title}</b>."
                "<br><br>"
                "Drop a folder containing a <code>manifest.json</code> into the "
                "mods folder (button below), then hit Refresh.<br>"
                "IPS and BPS patches both work - that is what Oracles "
                "randomizers and romhacks already ship as."
            )
            hint.setWordWrap(True)
            hint.setTextFormat(Qt.TextFormat.RichText)
            box_layout.addWidget(hint)
        else:
            for m in mods:
                label = f"{m['name']}  ·  v{m['version']}"
                if m["patch"]:
                    label += f"  ·  {Path(m['patch']).suffix.lstrip('.').upper()}"
                if m["author"]:
                    label += f"  ·  {m['author']}"
                cb = QCheckBox(label)
                cb.setChecked(bool(m["enabled"]))
                box_layout.addWidget(cb)
                if m["description"]:
                    desc = QLabel(m["description"])
                    desc.setWordWrap(True)
                    desc.setStyleSheet(
                        "color: #8b97a2; font-size: 11px; margin-left: 28px;"
                    )
                    box_layout.addWidget(desc)
                self.boxes.append((m["id"], cb))
        box_layout.addStretch(1)
        self.scroll.setWidget(inner)

    def save(self) -> None:
        self.runner.write_state({mod_id: cb.isChecked() for mod_id, cb in self.boxes})
        self.accept()


# --------------------------------------------------------------------------
# updates
# --------------------------------------------------------------------------

DIALOG_STYLE = """
    QDialog { background: #12151a; }
    QLabel { color: #d6dde4; }
    QPushButton {
        background: #23303a; color: #dfe8ee; border: 0; padding: 8px 18px;
        border-radius: 6px;
    }
    QPushButton:hover { background: #2e414f; }
    QPushButton:disabled { background: #1b232b; color: #63707c; }
    QProgressBar {
        background: #1b232b; border: 0; border-radius: 5px; height: 10px;
        text-align: center; color: #8b97a2;
    }
    QProgressBar::chunk { background: #3f6f8f; border-radius: 5px; }
"""


class StreamDialog(QDialog):
    """The stream overlays: which layout, where the openings are, what OBS
    should be told, and the line the ticker shows.

    Everything here edits files in `stream/`, and every one of them stays
    editable by hand -- this is a convenience, not a new source of truth.
    Nothing is written until Apply, so a half-typed number never reaches a
    page that OBS is reading several times a second.
    """

    def __init__(self, folder: Path, parent=None):
        super().__init__(parent)
        self.folder = folder
        self.splits_folder = folder.parent / "splits"
        self.setWindowTitle("Stream overlays")
        self.setMinimumSize(660, 620)
        self.setStyleSheet(
            """
            QDialog { background: #12151a; }
            QLabel { color: #d6dde4; }
            QCheckBox { color: #d6dde4; font-size: 13px; }
            QCheckBox::indicator { width: 16px; height: 16px; }
            QComboBox, QSpinBox, QLineEdit {
                background: #1b2129; color: #dfe8ee; border: 1px solid #2b3540;
                border-radius: 5px; padding: 5px 8px; font-size: 13px;
            }
            QSpinBox { min-width: 92px; }
            QPushButton {
                background: #23303a; color: #dfe8ee; border: 0; padding: 8px 16px;
                border-radius: 6px;
            }
            QPushButton:hover { background: #2e414f; }
            QFrame[role="rule"] { background: #232c36; max-height: 1px; border: 0; }
            """
        )

        outer = QVBoxLayout(self)

        if not folder.is_dir():
            # Someone running from a tree without the folder, or an archive
            # unpacked oddly. Say which folder rather than failing blankly.
            msg = QLabel(
                f"No stream folder at:\n{folder}\n\n"
                "The overlays ship beside the player. Point OBS at the "
                "stream/ folder in whatever directory you run it from."
            )
            msg.setWordWrap(True)
            outer.addWidget(msg)
            close = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
            close.rejected.connect(self.reject)
            outer.addWidget(close)
            return

        outer.addWidget(self._layout_row())
        outer.addWidget(self._rule())
        outer.addWidget(self._obs_block())
        outer.addWidget(self._rule())
        outer.addWidget(self._switches_block())
        outer.addWidget(self._rule())
        outer.addWidget(self._now_block())
        outer.addStretch(1)

        self.note = QLabel("")
        self.note.setWordWrap(True)
        self.note.setStyleSheet("color: #8b97a2; font-size: 11px;")
        outer.addWidget(self.note)

        row = QHBoxLayout()
        open_btn = QPushButton("Open Stream Folder")
        open_btn.clicked.connect(self.open_folder)
        row.addWidget(open_btn)
        row.addStretch(1)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Apply
            | QDialogButtonBox.StandardButton.Close
        )
        buttons.button(QDialogButtonBox.StandardButton.Apply).clicked.connect(self.apply)
        buttons.rejected.connect(self.reject)
        row.addWidget(buttons)
        outer.addLayout(row)

        self.load_layout()

    # -- construction ------------------------------------------------
    def _rule(self) -> QFrame:
        line = QFrame()
        line.setProperty("role", "rule")
        line.setFrameShape(QFrame.Shape.HLine)
        return line

    def _layout_row(self) -> QWidget:
        w = QWidget()
        row = QHBoxLayout(w)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(QLabel("Layout"))
        self.picker = QComboBox()
        for layout in stream_config.LAYOUTS:
            self.picker.addItem(
                f"{layout.title}  ({layout.width}×{layout.height})", layout
            )
        self.picker.currentIndexChanged.connect(self.load_layout)
        row.addWidget(self.picker, 1)
        return w

    def _obs_block(self) -> QWidget:
        w = QWidget()
        col = QVBoxLayout(w)
        col.setContentsMargins(0, 0, 0, 0)

        self.path_label = QLabel("")
        self.path_label.setStyleSheet("color: #8b97a2; font-size: 11px;")
        self.path_label.setWordWrap(True)
        col.addWidget(self.path_label)

        self.grid = QGridLayout()
        self.grid.setHorizontalSpacing(10)
        self.spins: dict[str, dict[str, QSpinBox]] = {}
        for r, (prefix, title) in enumerate(
            (("box", "Game opening"), ("cam", "Camera opening"))
        ):
            self.grid.addWidget(QLabel(title), r, 0)
            self.spins[prefix] = {}
            for c, axis in enumerate("xywh"):
                spin = QSpinBox()
                spin.setRange(0, 4096)
                spin.setPrefix({"x": "x ", "y": "y ", "w": "w ", "h": "h "}[axis])
                spin.valueChanged.connect(self.refresh_summary)
                self.grid.addWidget(spin, r, c + 1)
                self.spins[prefix][axis] = spin
            snap = QPushButton("Snap")
            snap.setToolTip(
                "Round to a whole multiple of the Game Boy's 160×144, so flat "
                "mode stays pixel-crisp"
            )
            snap.clicked.connect(lambda _=False, p=prefix: self.snap(p))
            self.grid.addWidget(snap, r, 5)
        col.addLayout(self.grid)

        self.summary = QLabel("")
        self.summary.setWordWrap(True)
        self.summary.setStyleSheet("color: #9fb4c6; font-size: 12px;")
        col.addWidget(self.summary)

        row = QHBoxLayout()
        copy_rects = QPushButton("Copy OBS Numbers")
        copy_rects.clicked.connect(self.copy_numbers)
        copy_url = QPushButton("Copy Overlay Path")
        copy_url.clicked.connect(self.copy_path)
        row.addWidget(copy_rects)
        row.addWidget(copy_url)
        row.addStretch(1)
        col.addLayout(row)
        return w

    # The switch label and the tooltip that explains it, in the order
    # they read on screen. Keyed by what goes into config.js.
    SWITCH_LABELS = {
        "cam": ("Camera opening",
                "A second, 16:9 hole in the rail with a matching frame"),
        "guide": ("Alignment guide",
                  "Print each opening's rectangle over itself, for lining a "
                  "capture up in OBS. Turn it off before going live."),
        "timer": ("Run timer",
                  "The file's own clock, to hundredths"),
        "splits": ("Split list",
                   "The route from splits/<cart>.txt, with the segment you "
                   "are on marked"),
        "tracker": ("Item tracker",
                    "A grid of the run's items, lit as you collect them"),
        "inputs": ("Input display",
                   "The buttons being held"),
    }

    def _switches_block(self) -> QWidget:
        w = QWidget()
        col = QVBoxLayout(w)
        col.setContentsMargins(0, 0, 0, 0)

        grid = QGridLayout()
        grid.setHorizontalSpacing(24)
        self.switch_boxes: dict[str, QCheckBox] = {}
        for i, (key, (label, tip)) in enumerate(self.SWITCH_LABELS.items()):
            box = QCheckBox(label)
            box.setToolTip(tip)
            grid.addWidget(box, i // 2, i % 2)
            self.switch_boxes[key] = box
        col.addLayout(grid)

        hint = QLabel(
            "These apply to every layout at once, and to a browser source "
            "added as a local file — no URL query needed. Any of the last "
            "four puts the rail in run mode, where the ticker and the "
            "standing plaque step aside."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #8b97a2; font-size: 11px;")
        col.addWidget(hint)

        # LiveSplit is the player's setting rather than an overlay one: it
        # decides whether a split goes down a socket, not what gets drawn.
        row = QHBoxLayout()
        self.ls_box = QCheckBox("Send splits to LiveSplit")
        self.ls_box.setToolTip(
            "Start the server in LiveSplit first: right-click → Control → "
            "Start TCP Server. Loopback only, and a failed connection is "
            "silent, so leaving this on with LiveSplit closed costs nothing."
        )
        self.ls_port = QSpinBox()
        self.ls_port.setRange(1, 65535)
        self.ls_port.setPrefix("port ")
        row.addWidget(self.ls_box)
        row.addWidget(self.ls_port)
        row.addStretch(1)
        self.ls_state = QLabel("")
        self.ls_state.setStyleSheet("color: #8b97a2; font-size: 11px;")
        row.addWidget(self.ls_state)
        col.addLayout(row)
        return w

    def _now_block(self) -> QWidget:
        w = QWidget()
        form = QFormLayout(w)
        form.setContentsMargins(0, 0, 0, 0)
        self.now_edit = QLineEdit()
        self.now_edit.setMaxLength(120)
        self.now_edit.setPlaceholderText("chasing down the tree shapes")
        form.addRow(QLabel("Now building"), self.now_edit)
        return w

    # -- state -------------------------------------------------------
    @property
    def layout_info(self) -> stream_config.Layout:
        return self.picker.currentData()

    @property
    def layout_path(self) -> Path:
        return self.folder / self.layout_info.filename

    def load_layout(self) -> None:
        """Read the selected overlay, and the folder-wide settings."""
        info = self.layout_info
        self.path_label.setText(str(self.layout_path))
        try:
            rects = stream_config.read_rects(self.layout_path)
        except stream_config.StreamConfigError as exc:
            rects = {}
            self.note.setText(str(exc))

        for prefix, spins in self.spins.items():
            rect = rects.get(prefix)
            for axis, spin in spins.items():
                spin.blockSignals(True)
                spin.setEnabled(rect is not None)
                spin.setValue(getattr(rect, axis) if rect else 0)
                spin.blockSignals(False)
        for r in range(self.grid.rowCount()):
            item = self.grid.itemAtPosition(r, 5)
            if item and item.widget():
                item.widget().setEnabled(bool(rects))

        switches = stream_config.read_switches(self.folder)
        for key, box in self.switch_boxes.items():
            box.setChecked(switches.get(key, False))

        # splits/ sits beside stream/, not inside it.
        on, port = stream_config.read_livesplit(self.splits_folder)
        self.ls_box.setChecked(on)
        self.ls_port.setValue(port)
        have = (self.splits_folder / f"{'tlozooa'}.txt").exists()
        self.ls_state.setText("" if have else "no split routes found")
        self.now_edit.setText(stream_config.read_now(self.folder))
        self.refresh_summary()

    def rect_of(self, prefix: str) -> stream_config.Rect | None:
        spins = self.spins[prefix]
        if not spins["w"].isEnabled():
            return None
        return stream_config.Rect(**{a: s.value() for a, s in spins.items()})

    def refresh_summary(self) -> None:
        info = self.layout_info
        if not info.framed:
            self.summary.setText(
                "This layout floats over your capture — there is nothing to "
                "line up. Add it as a browser source and you are done."
            )
            return
        lines = []
        for prefix, title in (("box", "Game capture"), ("cam", "Camera")):
            rect = self.rect_of(prefix)
            if rect is None:
                continue
            complaint = stream_config.validate(
                rect, info.canvas, title.lower()
            )
            lines.append(f"{title}: {rect.describe()}" + (f"  ⚠ {complaint}" if complaint else ""))
        self.summary.setText("\n".join(lines))

    # -- actions -----------------------------------------------------
    def snap(self, prefix: str) -> None:
        rect = self.rect_of(prefix)
        if rect is None:
            return
        snapped = stream_config.snap(rect, self.layout_info.canvas)
        for axis, spin in self.spins[prefix].items():
            spin.setValue(getattr(snapped, axis))

    def obs_numbers(self) -> str:
        info = self.layout_info
        parts = [f"{info.title} — canvas {info.width} × {info.height}"]
        for prefix, title in (("box", "Game capture"), ("cam", "Camera")):
            rect = self.rect_of(prefix)
            if rect is not None:
                parts.append(f"{title}: {rect.describe()}")
        return "\n".join(parts)

    def copy_numbers(self) -> None:
        QApplication.clipboard().setText(self.obs_numbers())
        self.note.setText("Copied. Paste into OBS's Edit → Transform box.")

    def copy_path(self) -> None:
        QApplication.clipboard().setText(str(self.layout_path))
        self.note.setText(
            "Copied. In OBS: Sources → + → Browser, tick Local file, paste."
        )

    def apply(self) -> None:
        """Write geometry, switches and the status line -- or none of them.

        The rectangles are checked before anything is written, because a
        box hanging off the canvas is a black bar on stream and the file
        would already be saved by the time it showed up.
        """
        info = self.layout_info
        rects: dict[str, stream_config.Rect] = {}
        for prefix, title in (("box", "game opening"), ("cam", "camera opening")):
            rect = self.rect_of(prefix)
            if rect is None:
                continue
            complaint = stream_config.validate(rect, info.canvas, title)
            if complaint:
                QMessageBox.warning(self, "That will not fit", complaint)
                return
            rects[prefix] = rect

        try:
            if rects:
                stream_config.write_rects(self.layout_path, rects)
            stream_config.write_switches(
                self.folder,
                {k: b.isChecked() for k, b in self.switch_boxes.items()},
            )
            stream_config.write_now(self.folder, self.now_edit.text())
            self.splits_folder.mkdir(parents=True, exist_ok=True)
            stream_config.write_livesplit(
                self.splits_folder, self.ls_box.isChecked(), self.ls_port.value()
            )
        except stream_config.StreamConfigError as exc:
            QMessageBox.warning(self, "Could not save", str(exc))
            return

        what = "Saved." if rects else "Saved the switches and the ticker."
        self.note.setText(
            f"{what} A live overlay picks this up within a few seconds — "
            "no need to touch OBS."
        )

    def open_folder(self) -> None:
        """Open stream/ in the file manager, so OBS can be pointed at it."""
        if os.name == "nt":
            os.startfile(str(self.folder))  # noqa: S606 - opening our own folder
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(self.folder)])
        else:
            subprocess.Popen(["xdg-open", str(self.folder)])


class UpdateDialog(QDialog):
    """Check for a newer release, then download and unpack it in place.

    The work happens on plain threads and reports back through signals; Qt
    delivers those to the widgets on the UI thread, which is the only place
    they may be touched.
    """

    found = Signal(object)          # updater.Update | None
    failed = Signal(str)
    progressed = Signal(int, int)   # bytes so far, bytes total
    installed = Signal()

    def __init__(self, install_dir: Path, known: object = None, parent=None):
        super().__init__(parent)
        self.install_dir = install_dir
        self.update: updater.Update | None = None
        self.cancel = threading.Event()
        self.busy = False

        self.setWindowTitle("Updates")
        self.setMinimumSize(560, 420)
        self.setStyleSheet(DIALOG_STYLE)

        layout = QVBoxLayout(self)

        self.status = QLabel()
        self.status.setWordWrap(True)
        self.status.setStyleSheet("font-size: 14px;")
        layout.addWidget(self.status)

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setStyleSheet("QScrollArea { border: 0; }")
        self.notes = QLabel()
        self.notes.setWordWrap(True)
        self.notes.setAlignment(Qt.AlignmentFlag.AlignTop)
        self.notes.setTextInteractionFlags(Qt.TextInteractionFlag.TextBrowserInteraction)
        self.notes.setOpenExternalLinks(True)
        self.notes.setStyleSheet("color: #a8b3bd; font-size: 12px;")
        self.scroll.setWidget(self.notes)
        layout.addWidget(self.scroll, 1)

        self.bar = QProgressBar()
        self.bar.setTextVisible(False)
        self.bar.hide()
        layout.addWidget(self.bar)

        self.footnote = QLabel(
            f"Installed version v{APP_VERSION}.  Your ROMs, mods, covers and "
            "saves are left untouched by an update."
        )
        self.footnote.setWordWrap(True)
        self.footnote.setStyleSheet("color: #8b97a2; font-size: 11px;")
        layout.addWidget(self.footnote)

        row = QHBoxLayout()
        self.install_btn = QPushButton("Download and install")
        self.install_btn.setEnabled(False)
        self.install_btn.clicked.connect(self.start_install)
        self.recheck_btn = QPushButton("Check again")
        self.recheck_btn.clicked.connect(self.start_check)
        row.addWidget(self.install_btn)
        row.addWidget(self.recheck_btn)
        row.addStretch(1)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        row.addWidget(buttons)
        layout.addLayout(row)

        self.found.connect(self.on_found)
        self.failed.connect(self.on_failed)
        self.progressed.connect(self.on_progress)
        self.installed.connect(self.on_installed)

        if isinstance(known, updater.Update):
            # The startup check already found this; no need to ask again.
            self.on_found(known)
        else:
            self.start_check()

    # -- checking --------------------------------------------------------

    def start_check(self) -> None:
        if self.busy:
            return
        self.busy = True
        self.install_btn.setEnabled(False)
        self.recheck_btn.setEnabled(False)
        self.status.setText("Checking for updates…")
        self.notes.setText("")

        def work() -> None:
            try:
                self.found.emit(updater.check(APP_VERSION))
            except updater.UpdateError as exc:
                self.failed.emit(str(exc))

        threading.Thread(target=work, daemon=True).start()

    def on_found(self, update: object) -> None:
        self.busy = False
        self.recheck_btn.setEnabled(True)
        if not isinstance(update, updater.Update):
            self.update = None
            self.status.setText(f"You are up to date on v{APP_VERSION}.")
            self.notes.setText("")
            return

        self.update = update
        size = update.asset_size / (1024 * 1024)
        self.status.setText(f"v{update.version} is available  ·  {size:.0f} MB")
        self.notes.setText(
            f"{update.notes}\n\n{update.page_url}" if update.notes else update.page_url
        )
        if FROZEN:
            self.install_btn.setEnabled(True)
        else:
            # Overwriting a git checkout with a release archive would be a
            # rude surprise. Say so instead of offering the button.
            self.footnote.setText(
                "This launcher is running from a source checkout, so it will "
                "not install over itself. Update with: git pull"
            )

    def on_failed(self, message: str) -> None:
        self.busy = False
        self.recheck_btn.setEnabled(True)
        self.bar.hide()
        self.status.setText("Could not check for updates.")
        self.notes.setText(f"{message}\n\n{updater.RELEASES_PAGE}")

    # -- installing ------------------------------------------------------

    def start_install(self) -> None:
        if self.busy or self.update is None:
            return
        self.busy = True
        self.cancel.clear()
        self.install_btn.setEnabled(False)
        self.recheck_btn.setEnabled(False)
        self.bar.setValue(0)
        self.bar.show()
        self.status.setText(f"Downloading v{self.update.version}…")

        update = self.update
        install_dir = self.install_dir

        def work() -> None:
            staging = None
            try:
                staging = Path(tempfile.mkdtemp(prefix=".download-", dir=install_dir))
                archive = updater.download(
                    update,
                    staging,
                    progress=lambda done, total: self.progressed.emit(done, total),
                    cancelled=self.cancel.is_set,
                )
                updater.install(archive, install_dir)
                self.installed.emit()
            except updater.UpdateError as exc:
                self.failed.emit(str(exc))
            except OSError as exc:
                self.failed.emit(f"Could not write to {install_dir}: {exc}")
            finally:
                if staging is not None:
                    shutil.rmtree(staging, ignore_errors=True)

        threading.Thread(target=work, daemon=True).start()

    def on_progress(self, done: int, total: int) -> None:
        if total > 0:
            self.bar.setRange(0, total)
            self.bar.setValue(done)
        else:
            self.bar.setRange(0, 0)  # unknown length: let it sweep

    def on_installed(self) -> None:
        self.busy = False
        self.bar.hide()
        version = self.update.version if self.update else ""
        self.status.setText(f"v{version} installed. Close and reopen to run it.")
        self.notes.setText(
            "The new build is in place. The launcher you are looking at is "
            "still the old one until you restart it."
        )
        self.footnote.setText(
            "Your ROMs, mods, covers and saves were left exactly as they were."
        )

    def reject(self) -> None:
        self.cancel.set()
        super().reject()


# --------------------------------------------------------------------------
# window
# --------------------------------------------------------------------------


class MainWindow(QWidget):
    update_found = Signal(object)   # updater.Update, from the startup check

    def __init__(self, runner: Runner):
        super().__init__()
        self.runner = runner
        self.pending_update: updater.Update | None = None
        self.setWindowTitle("Epoch & Equinox")
        self.resize(1180, 660)

        try:
            games = runner.query_games()
        except Exception as exc:  # noqa: BLE001 - surfaced to the user below
            QMessageBox.critical(
                self,
                "Cannot read the game list",
                f"Could not run:\n{runner.exe}\n\n{exc}\n\n"
                "Build it first:\n  cmake --build build -j",
            )
            raise SystemExit(1) from exc

        self.view = LauncherView(runner, games)
        self.view.action.connect(self.on_action)

        # Optional: navigate the launcher with a controller. The game handles
        # its own pad input; this is only for these menus.
        self.pad = GamepadBridge(self)
        if self.pad.available:
            self.pad.moved.connect(self.view.navigate)
            self.pad.accepted.connect(self.view.activate)
            self.pad.cancelled.connect(self.close)
            self.pad.status.connect(self.view.set_pad_name)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.view)
        self.view.setFocus()

        self.update_found.connect(self.on_update_found)
        self.start_update_check()

    def reload_games(self) -> None:
        try:
            self.view.refresh(self.runner.query_games())
        except Exception as exc:  # noqa: BLE001
            print(f"[launcher] refresh failed: {exc}", file=sys.stderr)

    def start_update_check(self) -> None:
        """Ask about updates in the background, and fail quiet if offline.

        Nothing here interrupts the launcher: a hit adds a line to the
        footer, and every failure -- no network, GitHub down, a proxy
        answering with nonsense -- is swallowed. The Updates menu item is
        where someone who wants an answer goes.
        """

        def work() -> None:
            try:
                found = updater.check(APP_VERSION, timeout=6.0)
            except updater.UpdateError:
                return
            except Exception:  # noqa: BLE001 - a background check never bites
                return
            if found is not None:
                self.update_found.emit(found)

        threading.Thread(target=work, daemon=True).start()

    def on_update_found(self, update: object) -> None:
        if not isinstance(update, updater.Update):
            return
        self.pending_update = update
        self.view.set_update_note(f"v{update.version} available")

    def on_action(self, item: str, game: Game | None) -> None:
        if item == "Exit":
            self.close()
            return
        if item == "Updates":
            # The only item that does not need a playable game behind it.
            UpdateDialog(PROJECT_ROOT, self.pending_update, self).exec()
            return
        if item == "Stream":
            # Also independent of any game: the overlays are files on disk.
            StreamDialog(PROJECT_ROOT / "stream", self).exec()
            return
        if game is None:
            return

        if item == "Install ROM":
            self.install_rom(game)
        elif item == "Achievements":
            AchievementsDialog(self.runner, game, self).exec()
        elif item == "Secrets":
            SecretsDialog(self.runner, game, self).exec()
        elif item == "Saves":
            SavesDialog(self.runner, game, self).exec()
        elif item == "Continue Legend":
            self.continue_legend(game)
        elif item == "Mods":
            if game.playable:
                ModsDialog(self.runner, game, self).exec()
        elif item == "Start game":
            if game.playable:
                self.start_game(game)

    def continue_legend(self, game: Game) -> None:
        """The seamless linked game: take this game's transfer secret and
        hand it to the other cart, which enters it by itself.

        Writes states/handoff.txt -- the queue the player checks at boot
        -- then starts the other game. Everything after that is the
        in-game machine: file select, SECRETS, the typist, and a linked
        file standing in a room. One real button press cancels it."""
        other_id = GAME_PAIR.get(game.id)
        other = next((g for g in self.view.games if g.id == other_id), None)
        if other is None or not other.playable:
            QMessageBox.information(
                self, "The other half is missing",
                "Continuing the legend needs the other game's ROM "
                "installed — it starts a linked game there.")
            return
        sav = oracle_secrets.find_saves(self.runner.root).get(game.id)
        saves = oracle_secrets.read_save(sav) if sav else []
        if not saves:
            QMessageBox.information(
                self, "No saved legend yet",
                f"There is no {game.title} save to continue from. "
                "Finish (or at least start) a quest here first.")
            return
        # A hero's file first, a linked file second, file 1 otherwise --
        # the file most likely to be the finished quest.
        source = sorted(saves, key=lambda s: (not s.is_hero,
                                              not s.is_linked, s.slot))[0]
        tgt = oracle_secrets.find_saves(self.runner.root).get(other_id)
        used = {s.slot for s in oracle_secrets.read_save(tgt)} if tgt else set()
        free = next((i for i in range(3) if i not in used), None)
        if free is None:
            QMessageBox.information(
                self, "No room in the other game",
                f"All three {other.title} files are in use. Erase or "
                "export one first (the Saves page can back it up).")
            return
        answer = QMessageBox.question(
            self, "Continue the legend?",
            f"{other.title} will start and enter {source.hero_name}'s "
            f"transfer secret by itself — file select, code entry, all "
            f"of it — leaving a linked game in file {free + 1}.\n\n"
            "Touch nothing while it types. Pressing any button hands "
            "control back to you and stops the machine.")
        if answer != QMessageBox.StandardButton.Yes:
            return
        symbols = oracle_secrets.game_secret(source)
        states = self.runner.root / "states"
        states.mkdir(exist_ok=True)
        (states / "handoff.txt").write_text(
            f"to={other_id}\nslot={free}\nname={source.hero_name}\n"
            "symbols=" + "".join(f"{v:02x}" for v in symbols) + "\n",
            encoding="utf-8")
        self.start_game(other)

    def install_rom(self, game: Game) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, f"Select the {game.title} ROM", "", "Game Boy ROMs (*.gbc *.gb);;All files (*)"
        )
        if not path:
            return
        try:
            self.runner.install_rom(game, Path(path))
        except OSError as exc:
            QMessageBox.warning(self, "Could not install ROM", str(exc))
            return
        # The runner does the real SHA check on first boot; this only reports
        # whether the file landed where it belongs.
        self.reload_games()

    def start_game(self, game: Game) -> None:
        try:
            self.game_proc = self.runner.launch(game)
        except OSError as exc:
            QMessageBox.warning(self, "Could not start the game", str(exc))
            return
        # Stay hidden for as long as the game owns the screen. The old
        # behaviour -- reappear on a fixed 1.5s timer -- put the launcher
        # window on top of the running game, which read as a glitch.
        self.hide()
        self.watch = QTimer(self)
        self.watch.setInterval(700)
        self.watch.timeout.connect(self.check_game)
        self.watch.start()

    def check_game(self) -> None:
        proc = getattr(self, "game_proc", None)
        if proc is None:
            self.watch.stop()
            return
        rc = proc.poll()
        if rc is None:
            return                       # still playing; stay out of the way
        self.watch.stop()
        self.game_proc = None
        self.reload_games()
        self.show()
        if rc != 0:
            # The runner never took (or lost) the screen -- surface what it
            # said instead of silently reappearing.
            tail = ""
            try:
                text = self.runner.run_log.read_text(
                    encoding="utf-8", errors="replace"
                ).strip()
                tail = "\n".join(text.splitlines()[-15:])
            except OSError:
                pass
            QMessageBox.critical(
                self,
                "The game exited",
                f"{self.runner.exe.name} exited with code {rc}.\n\n"
                f"{tail or '(no output captured)'}\n\n"
                f"Full log: {self.runner.run_log}",
            )


def default_runner_path() -> Path:
    here = Path(__file__).resolve().parent
    exe = "epoch.exe" if os.name == "nt" else "epoch"
    candidates = (
        (PROJECT_ROOT / exe, Path.cwd() / exe)
        if FROZEN
        else (here.parent / "build" / exe, here.parent / exe, Path.cwd() / exe)
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return PROJECT_ROOT / exe if FROZEN else here.parent / "build" / exe


def main() -> int:
    parser = argparse.ArgumentParser(description="Epoch & Equinox launcher")
    parser.add_argument(
        "--runner", type=Path, default=None, help="path to the oracles game binary"
    )
    parser.add_argument("--smoke-test", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setApplicationName("Epoch & Equinox")

    runner_path = args.runner or default_runner_path()
    if not runner_path.is_file():
        # This is the single most likely first-run failure: source downloaded,
        # nothing built yet. It used to print to stderr and exit, which looks
        # exactly like "the launcher didn't start" if you double-clicked it.
        message = (
            f"No game binary at:\n{runner_path}\n\n"
            "The games have to be compiled once before the launcher can run "
            "them. From the project folder:\n\n"
            "    cmake -S . -B build -G Ninja\n"
            "    cmake --build build -j\n\n"
            "That fetches Oracle of Seasons and the runtime, then builds both "
            "carts. It takes a while the first time and needs CMake, Ninja, "
            "SDL2 and libcurl installed.\n\n"
            "Already built somewhere else? Point at it with:\n"
            "    python3 launcher/epoch_launcher.py --runner /path/to/oracles"
        )
        print(f"[launcher] {message}", file=sys.stderr)
        box = QMessageBox()
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle("Oracles - not built yet")
        box.setText("The games haven't been built yet.")
        box.setInformativeText(message)
        box.exec()
        return 1

    runner = Runner(runner_path)
    if args.smoke_test:
        runner.query_games()
        return 0

    if FROZEN:
        # An update that had to rename a locked executable out of the way
        # leaves it behind; this is the next start it was waiting for.
        updater.sweep_old_files(PROJECT_ROOT)

    window = MainWindow(runner)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
