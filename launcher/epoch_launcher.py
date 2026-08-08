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
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# The repo root, for art bundled with the project itself. The launcher lives
# in launcher/, so its parent is the checkout.
PROJECT_ROOT = Path(__file__).resolve().parent.parent

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
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from gamepad import GamepadBridge

APP_VERSION = "1.0.0"

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

    def launch(self, game: Game) -> subprocess.Popen:
        return subprocess.Popen([str(self.exe), "--game", game.id], cwd=self.root)


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

MENU_ITEMS = ["Start game", "Mods", "Install ROM", "Exit"]


class LauncherView(QWidget):
    action = Signal(str, object)  # (menu item, Game)

    def __init__(self, runner: Runner, games: list[Game]):
        super().__init__()
        self.runner = runner
        self.games = games
        self.active = 0
        self.menu_index = 0
        self.pad_name = ""
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
            pr.setOpacity(0.78 if active else 0.24)
            pr.drawPixmap(
                int(cx - scaled.width() / 2), int(h / 2 - scaled.height() / 2), scaled
            )
            pr.setOpacity(1.0)

            # Scrim under the text side. Custom art is drawn bright enough to
            # actually look at, so the title and menu need their own contrast
            # rather than relying on the art being dim.
            scrim = QLinearGradient(0, 0, w, 0)
            dark = QColor(0, 0, 0, 205)
            clear = QColor(0, 0, 0, 0)
            if index == 0:
                scrim.setColorAt(0.0, dark)
                scrim.setColorAt(0.55, clear)
            else:
                scrim.setColorAt(0.45, clear)
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
        pr.setPen(sub)
        pr.drawText(QRectF(x, h * 0.13, box_w, 22), align, "EPOCH & EQUINOX")

        pr.setFont(title_font(30 if active else 27, QFont.Weight.Light))
        pr.setPen(ink)
        pr.drawText(QRectF(x, h * 0.16, box_w, 64), align, game.title)

        if not game.playable:
            warn = QColor(236, 150, 140)
            warn.setAlphaF(0.95 if active else 0.4)
            pr.setFont(label_font(8.0, 2.6))
            pr.setPen(warn)
            pr.drawText(QRectF(x, h * 0.245, box_w, 20), align, "ROM REQUIRED")

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
            pr.setPen(col)
            pr.drawText(rect, align | Qt.AlignmentFlag.AlignVCenter, f"{item}  ·")


# --------------------------------------------------------------------------
# mods dialog
# --------------------------------------------------------------------------


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
        mods = runner.scan_mods(game.id)

        if not mods:
            hint = QLabel(
                f"Nothing in <code>mods/</code> applies to <b>{game.title}</b>.<br><br>"
                "Drop a folder containing a <code>manifest.json</code> into "
                f"<code>{runner.mods_dir}</code>.<br>"
                "IPS and BPS patches both work - that is what Oracles "
                "randomizers and romhacks already ship as."
            )
            hint.setWordWrap(True)
            hint.setTextFormat(Qt.TextFormat.RichText)
            layout.addWidget(hint)
        else:
            inner = QWidget()
            box_layout = QVBoxLayout(inner)
            for m in mods:
                label = f"{m['name']}  ·  v{m['version']}"
                if m["patch"]:
                    label += f"  ·  {Path(m['patch']).suffix.lstrip('.').upper()}"
                cb = QCheckBox(label)
                cb.setChecked(bool(m["enabled"]))
                box_layout.addWidget(cb)
                self.boxes.append((m["id"], cb))
            box_layout.addStretch(1)
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(inner)
            scroll.setStyleSheet("QScrollArea { border: 0; }")
            layout.addWidget(scroll)

            note = QLabel(
                "Changes apply on the next launch. The stock ROM is kept as a "
                "pristine snapshot, so turning a mod off fully undoes it."
            )
            note.setWordWrap(True)
            note.setStyleSheet("color: #8b97a2; font-size: 11px;")
            layout.addWidget(note)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def save(self) -> None:
        self.runner.write_state({mod_id: cb.isChecked() for mod_id, cb in self.boxes})
        self.accept()


# --------------------------------------------------------------------------
# window
# --------------------------------------------------------------------------


class MainWindow(QWidget):
    def __init__(self, runner: Runner):
        super().__init__()
        self.runner = runner
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

    def reload_games(self) -> None:
        try:
            self.view.refresh(self.runner.query_games())
        except Exception as exc:  # noqa: BLE001
            print(f"[launcher] refresh failed: {exc}", file=sys.stderr)

    def on_action(self, item: str, game: Game | None) -> None:
        if item == "Exit":
            self.close()
            return
        if game is None:
            return

        if item == "Install ROM":
            self.install_rom(game)
        elif item == "Mods":
            if game.playable:
                ModsDialog(self.runner, game, self).exec()
        elif item == "Start game":
            if game.playable:
                self.start_game(game)

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
            self.runner.launch(game)
        except OSError as exc:
            QMessageBox.warning(self, "Could not start the game", str(exc))
            return
        self.hide()
        # Come back once the cart has had a moment to take the screen; the
        # runner owns its own window from here.
        QTimer.singleShot(1500, self.show_again)

    def show_again(self) -> None:
        self.reload_games()
        self.show()


def default_runner_path() -> Path:
    here = Path(__file__).resolve().parent
    exe = "epoch.exe" if os.name == "nt" else "epoch"
    for candidate in (here.parent / "build" / exe, here.parent / exe, Path.cwd() / exe):
        if candidate.is_file():
            return candidate
    return here.parent / "build" / exe


def main() -> int:
    parser = argparse.ArgumentParser(description="Epoch & Equinox launcher")
    parser.add_argument(
        "--runner", type=Path, default=None, help="path to the oracles game binary"
    )
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

    window = MainWindow(Runner(runner_path))
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
