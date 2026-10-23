"""The front end's part of the interface pack (`build_ui_pack.py`): the main
menu, difficulty, character creation, Load Character and the loading screen
(CEGUI `.uilayout` files), and the Pause Menu, Options and Help (`[UI]`).

The console's screens are the reference for the arrangement. Kept exactly:
every window's name (the code finds most of them by name, the CEGUI exporter's
`i_4xx` included), its type, and what the code acts on (`onClick`, `tabOrder`,
`Visible`, `Alpha`). The numbers, the art and the words are the pack's own.

The menu and character-creation readers look an image up in the sheet its
reference names (`set:widgets1 image:a_button`), so the CEGUI writer is given
the sheet each sprite was packed on.
"""
from xml.sax.saxutils import quoteattr

from PIL import ImageFont

from build_ui_pack import GOLD, INK, INK_DIM, PANEL, PANEL_DARK, Canvas, at, full, obj, over
from ui_pack_menu import glyph, outline_box

PROMPT = "FFFCD615"


# --------------------------------------------------------------------------
# Sprites.
# --------------------------------------------------------------------------









def stick(w, h, letter, arrows):
    c = Canvas(w, h)
    cx, cy, r = w / 2, h / 2, min(w, h) * 0.32
    c.ellipse(cx - r, cy - r, cx + r, cy + r, fill=PANEL_DARK, outline=INK, width=1.6)
    c.text(cx, cy + 0.5, letter, r * 0.9)
    a = r * 0.4
    if "u" in arrows:
        c.polygon([(cx - a, cy - r - 2), (cx, cy - r - 2 - a * 1.4), (cx + a, cy - r - 2)],
                  fill=INK)
    if "d" in arrows:
        c.polygon([(cx - a, cy + r + 2), (cx, cy + r + 2 + a * 1.4), (cx + a, cy + r + 2)],
                  fill=INK)
    return c.done()




def pet_icon(n, kind, state):
    c = Canvas(n, n)
    ink = {"normal": INK_DIM, "rollover": INK, "selected": GOLD}[state]
    k = n / 48.0
    if kind == "dog":
        c.ellipse(12 * k, 14 * k, 36 * k, 38 * k, outline=ink, width=2.4 * k)
        c.polygon([(10 * k, 12 * k), (17 * k, 10 * k), (14 * k, 28 * k)], fill=ink)
        c.polygon([(38 * k, 12 * k), (31 * k, 10 * k), (34 * k, 28 * k)], fill=ink)
    elif kind == "cat":
        c.ellipse(12 * k, 16 * k, 36 * k, 38 * k, outline=ink, width=2.4 * k)
        c.polygon([(13 * k, 21 * k), (14 * k, 6 * k), (22 * k, 17 * k)], fill=ink)
        c.polygon([(35 * k, 21 * k), (34 * k, 6 * k), (26 * k, 17 * k)], fill=ink)
    else:
        c.ellipse(10 * k, 14 * k, 38 * k, 40 * k, outline=ink, width=2.4 * k)
        c.line([(18 * k, 14 * k), (14 * k, 6 * k)], fill=ink, width=2.4 * k)
        c.line([(30 * k, 14 * k), (34 * k, 6 * k)], fill=ink, width=2.4 * k)
    c.ellipse(19 * k, 24 * k, 22 * k, 27 * k, fill=ink)
    c.ellipse(26 * k, 24 * k, 29 * k, 27 * k, fill=ink)
    return c.done()


def torch(w, h):
    c = Canvas(w, h)
    c.polygon([(w * 0.42, h * 0.45), (w * 0.58, h * 0.45), (w * 0.54, h * 0.95),
               (w * 0.46, h * 0.95)], fill=(90, 64, 34, 255), outline=INK_DIM, width=1.5)
    c.polygon([(w * 0.5, h * 0.05), (w * 0.68, h * 0.3), (w * 0.62, h * 0.44), (w * 0.38, h * 0.44),
               (w * 0.32, h * 0.3)], fill=(240, 150, 40, 255), outline=GOLD, width=1.5)
    return c.done()




def round_button(n, up):
    c = Canvas(n, n)
    c.ellipse(1.5, 1.5, n - 1.5, n - 1.5, fill=PANEL_DARK, outline=INK, width=1.6)
    m = n / 2
    d = n * 0.18
    if up:
        c.polygon([(m - d, m + d * 0.6), (m, m - d * 0.8), (m + d, m + d * 0.6)], fill=INK)
    else:
        c.polygon([(m - d, m - d * 0.6), (m, m + d * 0.8), (m + d, m - d * 0.6)], fill=INK)
    return c.done()


def slider_tab(w, h, lit):
    c = Canvas(w, h)
    c.rrect(3, 3, w - 3, h - 3, 7, fill=(60, 48, 26, 255) if lit else PANEL,
            outline=GOLD if lit else INK, width=2)
    return c.done()


def checkbox(n, lit):
    c = Canvas(n, n)
    c.rrect(n * 0.15, n * 0.15, n * 0.85, n * 0.85, n * 0.12, fill=PANEL_DARK,
            outline=GOLD if lit else INK, width=2)
    return c.done()


def tick(n):
    c = Canvas(n, n)
    c.line([(n * 0.28, n * 0.5), (n * 0.45, n * 0.68), (n * 0.74, n * 0.3)], fill=GOLD,
           width=n * 0.09)
    return c.done()


def controller(w, h):
    c = Canvas(w, h)
    k = w / 268.0
    c.rrect(40 * k, 30 * k, 228 * k, 120 * k, 40 * k, fill=PANEL, outline=INK, width=2.4 * k)
    c.ellipse(14 * k, 60 * k, 104 * k, 180 * k, fill=PANEL, outline=INK, width=2.4 * k)
    c.ellipse(164 * k, 60 * k, 254 * k, 180 * k, fill=PANEL, outline=INK, width=2.4 * k)
    c.rrect(60 * k, 40 * k, 208 * k, 110 * k, 30 * k, fill=PANEL)
    c.ellipse(64 * k, 54 * k, 96 * k, 86 * k, outline=INK, width=2 * k)
    c.ellipse(150 * k, 90 * k, 178 * k, 118 * k, outline=INK, width=2 * k)
    for (x, y), col in zip(((196, 50), (212, 66), (180, 66), (196, 82)),
                           ((230, 190, 50, 255), (214, 66, 56, 255), (56, 124, 222, 255),
                            (98, 186, 72, 255))):
        c.ellipse((x - 7) * k, (y - 7) * k, (x + 7) * k, (y + 7) * k, fill=col)
    return c.done()


def front_sprites():
    r = 2
    out = {
        "analog_button_left_ud": stick(36 * r, 48 * r, "L", "ud"),
        "analog_button_right_ud": stick(32 * r, 39 * r, "R", "ud"),
        "analog_button_left_down": stick(38 * r, 37 * r, "L", "d"),
        "loading_icon": torch(124, 150),
        "scroll_button_up_rollover": round_button(27 * r, True),
        "scroll_button_down_rollover": round_button(27 * r, False),
        "scroll_button_tab_rollover": round_button(27 * r, False),
        "sliderbar_vert_rollover": outline_box(43, 505, INK_DIM, radius=10),
        "slider_bar_rollover": outline_box(263, 38, GOLD, fill=PANEL_DARK, radius=12),
        "slider_tab": slider_tab(34 * r, 35 * r, False),
        "slider_tab_rollover": slider_tab(34 * r, 35 * r, True),
        "checkbox_off": checkbox(50 * r, False),
        "checkbox_on": checkbox(50 * r, True),
        "check": tick(50 * r),
        "controller": controller(268 * r, 194 * r),
    }
    for kind in ("dog", "cat", "chakawary"):
        out[kind] = pet_icon(48 * r, kind, "normal")
        out[f"{kind}_rollover"] = pet_icon(48 * r, kind, "rollover")
        out[f"{kind}_selected"] = pet_icon(48 * r, kind, "selected")
    return out


# The sprites some code looks up on one sheet by name, whatever a layout says:
# the difficulty screen's checkbox and its tick on `widgets2`. The rest of the
# front end's go on `widgets1`. What the PC's `Logo` and `GuiLook` have (the
# bars, the buttons, the class portraits, the panel looks) is not drawn at all.
# Console images the engine draws in the PC's like when the sheets lack them
# (`pc_image_for` in `render/menu_chrome.cpp`; keep the two lists the same). The
# pack does not draw these; a CEGUI file names the PC image itself instead.
PC_HAS = {
    "bag", "icon_defense", "icon_dexterity", "icon_magic", "icon_strength",
    "item_transmute_window", "pethealth", "petmana", "plus", "plus_glow",
    "quest_button_incomplete", "quest_button_rollover", "skills_window_active",
    "skills_window_passive", "slider_bar", "slider_bar_rollover", "slider_tab",
    "slider_tab_rollover", "spells_window", "spells_windowb", "stat_icon_defense",
    "stat_icon_dexterity", "stat_icon_magic", "stat_icon_strength", "traycolor_blank",
    "traycolor_blue", "traycolor_gold", "traycolor_green", "traycolor_red", "xbarfill",
    *(f"petstance_{kind}{state}" for kind in ("aggressive", "defensive", "passive")
      for state in ("", "_inactive", "_rollover")),
    *(f"pet_{kind}{state}" for kind in ("aggressive", "defensive", "passive")
      for state in ("", "_inactive")),
}

FIXED_SHEET = {
    "check": "widgets2",
    "checkbox_off": "widgets2",
    "checkbox_on": "widgets2",
}


# --------------------------------------------------------------------------
# CEGUI layouts. A window is (type, name, {property: value}, [children]); an
# image property's value is the image's name alone, and the writer adds the
# sheet it is on.
# --------------------------------------------------------------------------

IMAGE_PROPS = ("Image", "NormalImage", "HoverImage", "PushedImage")


def cw(kind, name, children=(), **props):
    """A CEGUI window. `kind` is the part after `ceguiWidget/`, or `Default`."""
    full_kind = "DefaultWindow" if kind == "Default" else f"ceguiWidget/{kind}"
    return (full_kind, name, props, list(children))


def rect(x0, y0, x1, y1):
    """`UnifiedAreaRect` from four edges, each an offset or a (scale, offset) pair."""
    def dim(v):
        s, o = v if isinstance(v, tuple) else (0, v)
        return f"{{{s:g},{o:g}}}"
    return "{" + ",".join(dim(v) for v in (x0, y0, x1, y1)) + "}"


def box(x, y, w, h):
    return rect(x, y, x + w, y + h)


FULL = rect(0, 0, (1, 0), (1, 0))


def write_cegui(path, windows, sheet_of):
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', "", "<GUILayout >"]

    def emit(win, depth):
        kind, name, props, children = win
        pad = "    " * depth
        lines.append(f"{pad}<Window Type={quoteattr(kind)} Name={quoteattr(name)} >")
        for key, value in props.items():
            if key in IMAGE_PROPS and value:
                # `Set:image` names a sheet outside the pack (the PC's `Loading`).
                sheet, _, picture = value.rpartition(":")
                if not sheet:
                    sheet = sheet_of.get(picture.lower())
                    if sheet is None:
                        raise SystemExit(f"{path}: no sheet has the image '{picture}'")
                value = f"set:{sheet} image:{picture}"
            value = str(value).replace("\n", "&#xa;")
            attr = quoteattr(value).replace("&amp;#xa;", "&#xa;")
            lines.append(f"{pad}    <Property Name={quoteattr(key)} Value={attr} />")
        for child in children:
            emit(child, depth + 1)
        lines.append(f"{pad}</Window>")

    for w in windows:
        emit(w, 1)
    lines.append("</GUILayout>")
    with open(path, "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("\n".join(lines) + "\n")


def static_text(name, words, area, font="Serif14", colour=None, horz=None, vert=None,
                outline=False, **props):
    p = {"Font": font, "Text": words, "UnifiedAreaRect": area}
    if colour:
        p["TextColour"] = colour
    if horz:
        p["HorzTextFormatting"] = horz
    if vert:
        p["VertFormatting"] = vert
    p.update(props)
    return cw("StaticTextOutline" if outline else "StaticText", name, **p)


def static_image(name, picture, area, children=(), **props):
    p = {"UnifiedAreaRect": area}
    if picture:
        p["Image"] = picture
    p.update(props)
    return cw("StaticImage", name, children, **p)


def image_button(name, area, on_click, tab_order, normal, hover, pushed, children=()):
    return cw("ImageButton", name, children, onClick=on_click, tabOrder=str(tab_order),
              NormalImage=normal, HoverImage=hover, PushedImage=pushed, UnifiedAreaRect=area)


RED = "GuiLook:ButtonStandardRed"
RED_LIT = "GuiLook:ButtonStandardRedHighlight"


def gem(name, label_name, words, y, on_click, tab_order):
    """A full-width menu button (the PC's red one) with its outlined label."""
    return image_button(name, box(0, y, 256, 46), on_click, tab_order, RED, RED_LIT, RED_LIT, [
        static_text(label_name, words, box(4, 10, 248, 28), "SerifBig", horz="CentreAligned",
                    outline=True),
    ])


def borders(names):
    """The frame every front-end screen has: top and bottom bands and their four
    corners, under the console's names for them."""
    top, top_left, top_right, bottom_left, bottom_right = names
    return [
        static_image(top, "Logo:topbar", rect((0.5, -512), 0, (0.5, 512), 140)),
        static_image(top_left, "Logo:topbarleft", box(0, 0, 225, 88)),
        static_image(top_right, "Logo:topbarright", box(0, 0, 225, 88),
                     HorizontalAlignment="Right"),
        static_image(bottom_left, "Logo:bottombarleft", box(0, 0, 225, 89),
                     VerticalAlignment="Bottom"),
        static_image(bottom_right, "Logo:bottombarright", box(0, 0, 225, 89),
                     VerticalAlignment="Bottom", HorizontalAlignment="Right"),
    ]


def prompt_group(name, glyph_name, glyph_image, glyph_area, text_name, words, x, w,
                 text_x=53, **text_props):
    return cw("Default", name, [
        static_image(glyph_name, glyph_image, glyph_area),
        static_text(text_name, words, box(text_x, 18, w - text_x, 24), colour=PROMPT,
                    **text_props),
    ], UnifiedAreaRect=box(x, 8, w, 61))


def select_group(name, text_name, glyph_name, stick_image="analog_button_left_ud"):
    """The stick's "Select" prompt at the left of the bar."""
    return cw("Default", name, [
        static_text(text_name, "Select", box(36, 9, 140, 51), colour=PROMPT,
                    vert="CentreAligned", horz="WordWrapLeftAligned"),
        static_image(glyph_name, stick_image, box(-4, 12, 36, 48)),
    ], UnifiedAreaRect=box(37, 2, 170, 71))


def bottom_bar_window(name, bar_name, children, width=834):
    return static_image(bar_name, "Logo:bottombar", box(0, 0, 1024, 89), [
        cw("Default", name, children, UnifiedAreaRect=box(101, -7, width, 74)),
    ], VerticalAlignment="Bottom", HorizontalAlignment="Centre")


def main_menu():
    column = cw("Default", "i_402", [
        gem("ContinueLast", "i_408", "Continue", 0, "guiContinueGame", 1),
        gem("NewCharacter", "i_412", "New Character", 52, "guiNewGameMenu", 2),
        gem("LoadCharacter", "i_410", "Load Character", 104, "guiContinueGameMenu", 3),
        gem("Achievements", "i_418", "Achievements", 156, "guiAchievementsMenu", 4),
        gem("Leaderboards", "i_404", "Leaderboards", 208, "guiLeaderboardMenu", 5),
        gem("Help", "i_406", "Help & Controls", 260, "GUIHELPANDOPTIONSMENU", 6),
        gem("i_415", "i_416", "Options", 312, "GUISETTINGSMENU", 7),
        gem("UnlockFullGame", "i_420", "Unlock Full Game", 364, "guiUnlockFullGameMenu", 8),
        gem("ExitGame", "i_414", "Quit Game", 416, "guiExitApplication", 9),
    ], UnifiedAreaRect=rect((-0.45, 140), 150, (-0.45, 396), 660),
        HorizontalAlignment="Centre")
    bar = bottom_bar_window("i_430", "i_429", [
        prompt_group("i_431", "i_432", "a_button", box(28, 19, 21, 21), "i_433", "Confirm",
                     172, 190),
        prompt_group("credits", "i_435", "start_button", box(28, 19, 29, 22), "i_436", "Credits",
                     625, 190, text_x=62),
        cw("Default", "invitefriend", [
            static_text("invitefriendtext", "Gift a Friend", box(53, 18, 210, 24),
                        colour=PROMPT),
            static_image("i_439", "button_x", box(28, 19, 21, 21)),
        ], UnifiedAreaRect=box(380, 8, 260, 61)),
        cw("Default", "i_440", [
            static_text("i_441", "Select", box(36, 9, 156, 51), colour=PROMPT,
                        vert="CentreAligned", horz="WordWrapLeftAligned"),
            static_image("i_442", "analog_button_left_ud", box(-4, 12, 36, 48)),
        ], UnifiedAreaRect=box(37, 2, 168, 71)),
    ])
    frame = cw("Default", "i_401", [
        column,
        *[w for w in borders(("i_425", "i_421", "i_423", "i_424", "i_426"))],
        static_text("DemoVersion", "Trial Version", box(0, 120, 255, 33), "SerifBig",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        static_text("CharacterModsWarning", "", box(766, 638, 255, 33), "SerifBig",
                    horz="RightAligned"),
        static_text("CopyrightInfo", "", box(240, 652, 464, 33), horz="RightAligned",
                    HorizontalAlignment="Centre"),
        bar,
    ], UnifiedAreaRect=FULL)
    credit_lines = [
        ("i_451", "Torchlight\n\nRunic Games, 2009", rect(0, 10, (0.23, 0), (1, 10))),
        ("i_452", "Xbox 360 release\n\nRunic Games, 2011", rect((0.5, -9), 10, (0.75, 18),
                                                                 (1, 10))),
        ("i_453", "This port\n\nTorchlight Rekindled", rect(0, 10, (0.25, 0), (1, 10))),
        ("i_454", "Engine\n\nOGRE, ParticleUniverse", rect((0.25, 0), 10, (0.48, 0), (1, 10))),
    ]
    credits = cw("Default", "CreditFrame", [
        static_image("i_444", "black", FULL, [
            cw("Default", "i_445", [
                static_image("i_446", "black", rect(0, 0, (0.9, 0), (0.85, 0)), [
                    static_image("i_447", "b_button", rect(-50, -7, -29, 14), [
                        static_text("i_448", "Close", box(25, 0, 250, 25)),
                    ], VerticalAlignment="Bottom", HorizontalAlignment="Right"),
                    cw("LineFrame", "i_450", [
                        static_text(name, words, area, "Serif", horz="WordWrapCentreAligned",
                                    **({"HorizontalAlignment": "Right"} if name == "i_453"
                                       else {}))
                        for name, words, area in credit_lines
                    ], tabOrder="0", UnifiedAreaRect=rect(0, -11, (1, 0), (1, 2))),
                ], VerticalAlignment="Centre", HorizontalAlignment="Centre"),
            ], UnifiedAreaRect=rect(0, 12, (1, 0), (1, -20))),
            static_text("Credits", "Credits", box(0, -320, 274, 61), horz="CentreAligned",
                        VerticalAlignment="Centre", HorizontalAlignment="Centre"),
        ], Alpha="0.9"),
    ], Visible="False", UnifiedAreaRect=FULL, VerticalAlignment="Centre",
        HorizontalAlignment="Centre")
    press_a = cw("Default", "PressAScreen", [
        cw("ItemText", "i_458", [
            cw("Default", "i_459", [
                cw("LineFrame", "FrameB", [
                    cw("StaticText", "i_462", [
                        static_image("i_463", "a_button", box(67, 22, 21, 21)),
                    ], Font="Serif14", Text="Begin", HorzTextFormatting="CentreAligned",
                        HorizontalAlignment="Centre", UnifiedAreaRect=box(0, 221, 158, 59)),
                    static_image("i_464", "loading_icon", box(0, 55, 124, 150),
                                 HorizontalAlignment="Centre"),
                ], tabOrder="0", UnifiedAreaRect=box(0, 0, 500, 270)),
            ], UnifiedAreaRect=box(0, 235, 500, 270), HorizontalAlignment="Centre"),
        ], tabOrder="0", UnifiedAreaRect=FULL),
    ], Visible="False", UnifiedAreaRect=FULL)
    return [static_image("i_135", None, FULL, [
        cw("Default", "i_400", [frame, credits, press_a], UnifiedAreaRect=FULL),
    ])]


def difficulty():
    bar = bottom_bar_window("i_4750", "i_4749", [
        prompt_group("i_4751", "i_4752", "a_button", box(28, 19, 21, 21), "i_4753", "Confirm",
                     172, 170),
        cw("Default", "i_4754", [
            static_text("i_4755", "Select", box(36, 9, 142, 51), colour=PROMPT,
                        vert="CentreAligned", horz="WordWrapLeftAligned"),
            static_image("i_4756", "analog_button_left_ud", box(-4, 12, 36, 48)),
        ], UnifiedAreaRect=box(37, 2, 175, 71)),
        prompt_group("i_4757", "i_4759", "b_button", box(45, 19, 21, 21), "i_4758", "Back", 280,
                     135, text_x=70),
    ], width=438)
    frame = cw("Default", "i_4745", [
        *borders(("i_4760", "i_4762", "i_4746", "i_4747", "i_4761")),
        static_text("DemoVersion", "Select Difficulty", box(0, 120, 255, 33), "SerifBig",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        bar,
    ], UnifiedAreaRect=FULL)
    panel = cw("Default", "i_4763", [
        cw("ItemText", "i_4765", [
            gem("ContinueLast", "i_4772", "Easy", 0, "GUISELECT1", 5),
            gem("NewCharacter", "i_4769", "Normal", 52, "GUISELECT2", 1),
            gem("LoadCharacter", "i_4776", "Hard", 104, "GUISELECT3", 2),
            gem("Achievements", "i_4767", "Very Hard", 156, "GUISELECT4", 3),
            cw("Checkbox", "Hardcore", onClick="", tabOrder="4",
               UnifiedAreaRect=box(4, 214, 50, 40)),
            static_text("i_4777", "Hardcore", box(58, 220, 300, 33), "SerifBig", outline=True),
            static_text("i_4770", "(death is permanent)", box(58, 242, 300, 33), outline=True),
        ], tabOrder="0", UnifiedAreaRect=box(0, 0, 262, 268)),
    ], UnifiedAreaRect=rect((-0.45, 150), 155, (-0.45, 444), 455), HorizontalAlignment="Centre")
    return [static_image("i_135", None, FULL, [
        frame, panel, cw("Default", "i_4778", UnifiedAreaRect=box(50, 155, 294, 496)),
    ])]




def character_create():
    def pet_box(box_name, pet, x, tab_order):
        return static_image(box_name, "GuiLook:SkillSlot", box(x, 228, 66, 66), [
            image_button(pet, box(9, 9, 48, 48), "GUISELECT2", tab_order, pet.lower(),
                         f"{pet.lower()}_rollover", f"{pet.lower()}_selected"),
        ])

    def heading(frame_name, text_name, words, y):
        return static_image(frame_name, None, box(53, y, 177, 41), [
            static_text(text_name, words, box(0, 11, 250, 20), "Serif", horz="CentreAligned",
                        HorizontalAlignment="Centre"),
        ])

    def name_box(frame_name, edit_name, y, select, tab_order, colour=None):
        props = {"TextColour": colour} if colour else {}
        return cw("ItemTooltip", frame_name, [
            cw("StaticText", edit_name, Font="Serif", onClick=select, tabOrder=str(tab_order),
               UnifiedAreaRect=box(0, 0, 209, 30), **props),
        ], tabOrder="0", UnifiedAreaRect=box(42, y, 197, 23))

    # The console lights the focused class with its HoverImage: the PC's bright-framed
    # `iconselected` is the Xbox's `_rollover`, and its blue `iconlit` the Xbox's `_selected`.
    classes = cw("ItemTooltip", "i_450", [
        image_button("Destroyer", box(-6, 40, 94, 94), "GUISELECT1", 1, "Logo:destroyericon",
                     "Logo:destroyericonselected", "Logo:destroyericonlit"),
        image_button("Alchemist", box(90, 40, 94, 94), "GUISELECT1", 2, "Logo:alchemisticon",
                     "Logo:alchemisticonselected", "Logo:alchemisticonlit"),
        image_button("Vanquisher", box(186, 40, 94, 94), "GUISELECT1", 3, "Logo:vanquishericon",
                     "Logo:vanquishericonselected", "Logo:vanquishericonlit"),
        heading("i_453", "i_454", "Select Class", 0),
    ], tabOrder="0", UnifiedAreaRect=box(3, 7, 274, 138))
    panel = cw("Default", "i_432", [
        cw("ItemText", "i_434", [
            classes,
            heading("i_447", "i_448", "Player Name", 146),
            name_box("i_443", "EditBox", 182, "GUISELECT49", 4),
            heading("i_440", "i_441", "Select Pet", 198),
            pet_box("i_435", "Dog", 40, 5),
            pet_box("i_445", "Cat", 106, 6),
            pet_box("i_458", "Chakawary", 172, 7),
            heading("i_461", "i_462", "Pet Name", 290),
            name_box("i_438", "EditBoxPet", 326, "GUISELECT50", 8, "FFFFFAFF"),
            image_button("DoneButton", box(30, 374, 220, 51), "GUINEWGAME", 9,
                         RED, RED_LIT, RED_LIT, [
                             static_text("i_457", "Done", box(0, -3, 217, 48), "Serif",
                                         horz="CentreAligned", vert="CentreAligned",
                                         VerticalAlignment="Centre",
                                         HorizontalAlignment="Centre"),
                         ]),
            static_text("AtLeastOneChar", "The player and pet names must be at least one "
                        "character", box(0, 382, 230, 35), "Serif", colour="FFFF4040",
                        horz="WordWrapCentreAligned", vert="BottomAligned", Visible="False",
                        HorizontalAlignment="Centre"),
        ], tabOrder="0", UnifiedAreaRect=box(3, -6, 279, 438)),
    ], UnifiedAreaRect=rect((-0.45, 155), 155, (-0.45, 449), 651), HorizontalAlignment="Centre")

    def award(name, text_name, words, y, h):
        return static_image(name, "socket_full", box(0, y, 18, 18), [
            static_text(text_name, words, box(26, 0, 230, h), "Serif", colour="FFFBCA04",
                        horz="WordWrapLeftAligned"),
        ])

    awards = cw("Default", "awards_window", [
        static_text("i_408", "AWARDS", box(0, 1, 250, 34), "SerifBig", colour="FFFBE204",
                    horz="CentreAligned", HorizontalAlignment="Centre"),
        cw("ItemText", "i_410", tabOrder="0", UnifiedAreaRect=rect(0, 0, (1, 0), (1, -29))),
        award("award_invited", "i_412", "You will receive a Potion of Respec", 29, 41),
        award("award_promotional", "i_414", "You will receive a Troll Transformation fish",
              126, 50),
        award("award_community", "i_416", "You will receive a bonus spell", 76, 43),
    ], UnifiedAreaRect=rect(-70, -407, 184, -196), VerticalAlignment="Bottom",
        HorizontalAlignment="Right")
    bar = bottom_bar_window("i_422", "i_421", [
        prompt_group("backbutton", "i_425", "b_button", box(45, 19, 21, 21), "i_424", "Back",
                     280, 135, text_x=70),
        prompt_group("i_426", "i_428", "a_button", box(28, 19, 21, 21), "i_427", "Confirm", 172,
                     158),
        cw("Default", "i_429", [
            static_image("i_430", "analog_button_left_lr", box(-30, 17, 67, 37)),
            static_text("i_431", "Select", box(36, 9, 131, 51), colour=PROMPT,
                        vert="CentreAligned", horz="WordWrapLeftAligned"),
        ], UnifiedAreaRect=box(37, 2, 145, 71)),
    ], width=438)
    frame = cw("Default", "i_400", [
        static_text("CharacterModsWarning", "", box(766, 638, 255, 33), "SerifBig",
                    horz="RightAligned"),
        static_text("CharacterClassDescription", "", box(0, 597, 448, 73),
                    horz="WordWrapCentreAligned", outline=True, HorizontalAlignment="Centre"),
        *borders(("i_404", "i_418", "i_405", "i_403", "i_419")),
        cw("Default", "i_406", UnifiedAreaRect=box(0, 86, 1024, 590)),
        awards,
        static_text("CharacterClass", "", box(0, 139, 255, 52), "SerifHuge",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        static_text("i_420", "Select Hero Class", box(0, 113, 255, 33), "SerifBig",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        bar,
    ], UnifiedAreaRect=FULL)
    return [static_image("i_135", None, FULL, [frame, panel])]


def character_load():
    def slot(n):
        return cw("ItemTooltip", f"Player{n}", [
            static_text(f"Player{n}Name", "Name", box(2, 7, 228, 26), "SerifBig",
                        onClick=f"guiSelect{n}", tabOrder=str(n)),
            static_text(f"Player{n}Desc", "Desc", box(24, 31, 208, 40), "Serif"),
            cw("ItemTooltip2", f"PlayerHighlight{n}", tabOrder="0",
               UnifiedAreaRect=box(0, 0, 221, 72)),
        ], tabOrder="0", UnifiedAreaRect=box(28, 11 + 81 * (n - 1), 221, 72))

    list_panel = cw("Default", "i_3982", [
        cw("ItemText", "i_3984", [
            static_image("ScrollUp", "scroll_button_up_rollover", box(126, -27, 27, 27)),
            *[slot(n) for n in range(1, 6)],
            static_image("highlight", "facet", box(-14, -16, 310, 124)),
            static_image("ScrollDown", "scroll_button_down_rollover", box(126, 430, 27, 27)),
        ], tabOrder="0", UnifiedAreaRect=box(3, -6, 279, 438)),
    ], UnifiedAreaRect=rect((-0.45, 160), 155, (-0.45, 454), 651), HorizontalAlignment="Centre")
    bar = bottom_bar_window("i_4021", "i_4020", [
        prompt_group("i_4022", "i_4024", "b_button", box(45, 19, 21, 21), "i_4023", "Back", 280,
                     135, text_x=70),
        cw("Default", "i_4025", [
            static_image("i_4026", "analog_button_left_ud", box(-4, 12, 36, 48)),
            static_text("i_4027", "Select", box(36, 9, 127, 51), colour=PROMPT,
                        vert="CentreAligned", horz="WordWrapLeftAligned"),
        ], UnifiedAreaRect=box(37, 2, 168, 71)),
        prompt_group("i_4028", "i_4029", "a_button", box(28, 19, 21, 21), "i_4030", "Confirm",
                     172, 161),
        prompt_group("i_4031", "i_4032", "button_x", box(67, 19, 21, 21), "i_4033", "View", 377,
                     199, text_x=92),
        prompt_group("i_4034", "i_4035", "y_button", box(67, 19, 21, 21), "i_4036", "Delete",
                     502, 236, text_x=92),
    ], width=812)
    frame = cw("Default", "i_3978", [
        static_text("i_3979", "Load a Character", box(0, 120, 255, 33), "SerifBig",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        *borders(("i_3981", "i_4019", "i_4038", "i_3980", "i_4018")),
        list_panel,
        bar,
        static_text("CharacterName", "", box(0, 612, 480, 51), "SerifHuge",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
    ], UnifiedAreaRect=FULL)

    def confirm(name, glyph_name, glyph_image, text_name, words, x):
        return cw("Default", name, [
            static_image(glyph_name, glyph_image, box(12, 19, 21, 21)),
            static_text(text_name, words, box(37, 18, 93, 24), colour=PROMPT),
        ], UnifiedAreaRect=box(x, 8, 135, 61))

    delete = cw("ItemText", "DeleteConfirm", [
        static_text("i_3964", "Delete Character?", box(0, 60, 480, 51), "SerifHuge",
                    horz="CentreAligned", outline=True, HorizontalAlignment="Centre"),
        cw("Default", "i_3965", [
            confirm("i_3966", "i_3967", "a_button", "i_3968", "Confirm", 91),
            confirm("i_3969", "i_3971", "b_button", "i_3970", "Cancel", 205),
        ], UnifiedAreaRect=box(0, 135, 438, 74), HorizontalAlignment="Centre"),
    ], Visible="False", tabOrder="0", UnifiedAreaRect=box(0, 221, 478, 267),
        HorizontalAlignment="Centre")
    dead = cw("ItemText", "DeadConfirm", [
        cw("Default", "i_3974", [
            static_image("i_3975", "a_button", box(12, 19, 21, 21)),
            static_text("i_3976", "OK", box(37, 18, 93, 24), colour=PROMPT),
        ], UnifiedAreaRect=box(27, 8, 135, 61), VerticalAlignment="Bottom",
            HorizontalAlignment="Centre"),
        static_text("i_3977", "This character was killed in hardcore mode. They are permanently "
                    "dead. You are not able to continue playing with them.", box(0, 10, 644, 245),
                    "SerifBig", horz="WordWrapCentreAligned", vert="CentreAligned", outline=True,
                    HorizontalAlignment="Centre"),
    ], Visible="False", tabOrder="0", UnifiedAreaRect=box(0, 221, 674, 333),
        HorizontalAlignment="Centre")
    return [static_image("i_135", None, FULL, [frame, delete, dead])]


# --------------------------------------------------------------------------
# The Pause Menu, Options and Help, in `[UI]`. Both the editor's `NAME` and the
# `WIDGET NAME` are kept: the Pause Menu finds its rows by `NAME`.
# --------------------------------------------------------------------------

def ui_header(words, w):
    """The plate across a frame's top edge, under the console's names for it."""
    return obj("Image", "Image1", [
        obj("Text Outlined", "Text Outlined4_clone14", widget="Title", TEXT=words, FONT="SerifBig",
            **{"HORIZONTAL ALIGN": "CENTER", "CLIPPED": False}, **at(0, 3, w, 26, ha="CENTER")),
    ], IMAGE="facet_panel_bottom", **at(0, -17, w + 30, 32, ha="CENTER"),
        **{"ALWAYS ON TOP": True, "CLIPPED": False})


def menu_row(name, text_name, words, y, widget=None, colour=None):
    """One of the Pause Menu's buttons, with its word."""
    props = {"widget": widget} if widget else {}
    word = {"COLOR": colour} if colour else {}
    return obj("Button", name, [
        obj("Text", text_name, TEXT=words, FONT="Serif14", **word,
            **{"HORIZONTAL ALIGN": "CENTER", "VERTICAL ALIGN": "CENTER"},
            **at(0, -3, 0, 0), **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
    ], **props, NORMAL="ButtonStandardRed", HOVER="ButtonStandardRedHighlight",
        CLICKED="ButtonStandardRedHighlight", **at(0, y, 220, 51, ha="CENTER"))


def pause_menu():
    return [
        obj("CEGUI Object", "CEGUI Object3_clone13", [
            menu_row("Settings", "Text1", "Options", 16),
            menu_row("HowToPlay", "Text1", "Help", 68),
            menu_row("Achievements", "Text1_clone309121", "Achievements", 120),
            menu_row("Leaderboards", "Text1_clone309092", "Leaderboards", 172,
                     widget="Leaderboards"),
            menu_row("ExitToTitle", "Text1_clone309647", "Exit to Title", 224),
            menu_row("unlockFullGame", "Text1_clone308481", "Unlock Full Game", 276,
                     widget="UnlockFullGame"),
            menu_row("invitefriend", "Text1_clone308481", "Gift a Friend", 340,
                     widget="invitefriend", colour=PROMPT),
            ui_header("Pause Menu", 200),
            obj("Window", "Window15_clone15", [
                obj("Window", "confirm_clone3", [
                    obj("Image", "a button_clone335555", IMAGE="start_button",
                        **at(7, 36, 29, 22)),
                    obj("Text", "Text26_clone335558", TEXT="Close", FONT="Serif14",
                        COLOR=PROMPT, **at(40, 36, 90, 24)),
                ], widget="Ok", **at(256, 8, 135, 61)),
            ], **at(50, 350, 438, 74, ha="CENTER")),
        ], widget="FrameB", **{"LOOK N FEEL": "ceguiWidget/ItemText"},
            **at(0, 126, 380, 420, ha="CENTER")),
    ]


def options():
    def slider(name, widget, tab_widget, words, x, y, tab_x, label_name="sound"):
        return obj("Button", name, [
            obj("Button", "slider tab", widget=tab_widget, NORMAL="SLIDER_TAB",
                DISABLED="SLIDER_TAB_ROLLOVER", HOVER="SLIDER_TAB_ROLLOVER",
                CLICKED="SLIDER_TAB_ROLLOVER", **at(tab_x, 0, 34, 35)),
            obj("Text", label_name, TEXT=words, FONT="Serif14",
                **{"HORIZONTAL ALIGN": "CENTER"}, **at(14, -18, 234, 22)),
        ], widget=widget, NORMAL="SLIDER_BAR", HOVER="SLIDER_BAR_ROLLOVER",
            CLICKED="SLIDER_BAR_ROLLOVER", **at(x, y, 263, 38))

    def check(name, widget, text_name, words, x, y, w=220):
        return obj("Check Box", name, [
            obj("Text", text_name, TEXT=words, FONT="Serif14", COLOR=PROMPT,
                **at(47, 8, w, 23)),
        ], widget=widget, **at(x, y))

    def wide_button(name, widget, words, y, arrows=False):
        children = [obj("Text", "Text24", TEXT=words, **{"HORIZONTAL ALIGN": "CENTER",
                                                          "VERTICAL ALIGN": "CENTER",
                                                          "WORD WRAP": True},
                        **at(30 if arrows else 0, -3, 160 if arrows else 210, 48,
                            "CENTER", "CENTER"))]
        if arrows:
            children += [
                obj("Image", "PickerLeftArrow", widget="PickerLeftArrow",
                    IMAGE="decor_arrow_left", **at(12, 11, 20, 27)),
                obj("Image", "PickerRightArrow", widget="PickerRightArrow",
                    IMAGE="decor_arrow_right", **at(188, 11, 20, 27)),
            ]
        return obj("Button", name, children, widget=widget,
            NORMAL="ButtonStandardRed", HOVER="ButtonStandardRedHighlight",
            CLICKED="ButtonStandardRedHighlight", **at(0, y, 220, 51))

    legend = obj("CEGUI Object", "InspectAButton", [
        obj("Image", "BButton_clone", IMAGE="analog_button_left_ud", **at(-8, 0, 36, 48,
                                                                           va="CENTER")),
        obj("Text", "ColumnName", TEXT="Select", FONT="Serif14", COLOR=PROMPT,
            **{"VERTICAL ALIGN": "CENTER"}, **at(30, -1, -630, 0),
            **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
        obj("Image", "dpad", IMAGE="dpad_leftright", **at(142, 7, 35, 35)),
        obj("Text", "ColumnName_clone", TEXT="Change Value", FONT="Serif14", COLOR=PROMPT,
            **{"VERTICAL ALIGN": "CENTER"}, **at(180, 0, -580, 0),
            **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
        obj("Image", "BButton_clone", IMAGE="a_button", **at(358, 0, 21, 21, va="CENTER")),
        obj("Text", "Top_clone", TEXT="Toggle Selection", FONT="Serif14", COLOR=PROMPT,
            **{"VERTICAL ALIGN": "CENTER"}, **at(384, 0, -586, 0),
            **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
        obj("Image", "BButton_clone", IMAGE="y_button", **at(560, 0, 21, 21, va="CENTER")),
        obj("Text", "Top", TEXT="Help", FONT="Serif14", COLOR=PROMPT,
            **{"VERTICAL ALIGN": "CENTER"}, **at(586, 0, -647, 0),
            **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
        obj("Image", "BButton", IMAGE="b_button", **at(668, 0, 21, 21, va="CENTER")),
        obj("Text", "Top_clone", TEXT="Done", FONT="Serif14", COLOR=PROMPT,
            **{"VERTICAL ALIGN": "CENTER"}, **at(694, 0, -604, 0),
            **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
    ], **{"LOOK N FEEL": "ceguiWidget/LineFrame"}, **at(2, 546, 772, 46))

    return [
        obj("Window", "Window", [
            obj("CEGUI Object", "CEGUI Object0", [
                obj("Window", "audio", [
                    slider("music slider", "MusicVolume", "sliderTabMusic", "Music Volume", 16,
                           12, 36, "music"),
                    check("Check Box20", "MusicMute", "Text25_clone25", "Mute Music", 16, 42),
                    slider("sound slider", "SoundVolume", "sliderTabSound", "Sound Volume", 16,
                           100, 86),
                    check("Check Box26", "SoundMute", "Text25_clone32", "Mute Sound", 16, 128),
                ], **at(33, 30, 370, 180)),
                obj("Window", "graphics", [
                    check("Check Box0", "ShowTips", "Text3_clone3", "Show Tips", 0, 4),
                    check("Check Box6", "ShowFloatyNumbers", "Text13_clone13",
                          "Show Damage Info", 0, 42, 300),
                    check("Check Box14", "ShowBlood", "Text16_clone16", "Show Blood", 0, 80),
                    slider("brightness slider", "Brightness", "sliderTabBrightness",
                           "Brightness", 0, 138, 86),
                    slider("contras slider_clone", "Contrast", "sliderTabContrast", "Contrast",
                           0, 186, 86),
                    wide_button("Button23", "Storage", "Set Storage Device", 224, arrows=True),
                    wide_button("Button23_clone", "Defaults", "Reset Defaults", 276),
                ], **at(50, 204, 720, 338)),
                legend,
                ui_header("Options", 160),
            ], **{"LOOK N FEEL": "ceguiWidget/ItemText"}, **at(0, 0, 772, 600)),
        ], **at(0, 90, 772, 602, ha="CENTER")),
        obj("Image", "Image", IMAGE="black", ALPHA=0.5, **full(),
            **{"VERTICLE": "CENTER", "HORIZONTAL": "CENTER"}),
    ]


HELP_TEXT = (
    "Explore the mines beneath the town of Torchlight, a little deeper on every floor. "
    "Fight what you find there, gather its gold and gear, and bring it home to trade. "
    "Learn skills as you level up, map them to the triggers and face buttons, and swap "
    "between two sets of them with the d-pad. Your pet fights at your side and can carry "
    "loot to town to sell for you."
)


def help_screen():
    def pair(image_name, picture, ix, iy, iw, ih, text_name, words, tx, ty, tw=234):
        return [obj("Image", image_name, IMAGE=picture, **at(ix, iy, iw, ih)),
                obj("Text", text_name, TEXT=words, **at(tx, ty, tw, 21))]

    controls = []
    for args in (("leftbumper", "left_bumper", 2, 2, 40, 24, "leftbumper text",
                  "Drink Health Potion", 47, 2),
                 ("rightbumper", "right_bumper", 2, 31, 40, 24, "rightbumer text",
                  "Drink Mana Potion", 47, 32),
                 ("lefttrigger", "left_trigger", 8, 60, 26, 40, "lefttrigger text",
                  "Skill - Assign from Skill Menu", 47, 60),
                 ("rightrigger", "right_trigger", 8, 103, 26, 40, "righttrigger text",
                  "Skill - Assign from Skill Menu", 47, 104),
                 ("DPadUpDown", "dpad_updown", 6, 155, 35, 35, "DPadUpDown",
                  "Switch Active Skill Set", 47, 164),
                 ("BackButton", "back_button", 286, 1, 29, 22, "backbutton text",
                  "Inventory/Submenus", 321, 3),
                 ("HomeButton", "start_button", 286, 26, 29, 22, "startbutton text",
                  "Options Menu", 319, 25),
                 ("YButton", "y_button", 500, 45, 21, 21, "YButton Text",
                  "Skill - Assign from Skill Menu", 526, 48, 253),
                 ("bbutton", "b_button", 500, 69, 21, 21, "BButtonText",
                  "Skill - Assign from Skill Menu", 526, 71, 251),
                 ("A Button", "a_button", 500, 92, 21, 21, "AButtonText",
                  "Interact / Pick Up Item", 526, 94),
                 ("Xbutton", "button_x", 500, 115, 21, 21, "X Button text", "Attack", 526, 116),
                 ("rstick", "analog_button_right_ud", 517, 145, 32, 39, "Text", "Zoom Camera",
                  556, 155, 149),
                 ("DPadLeft", "analog_button_left_down", 515, 192, 38, 37, "dPadLeft text",
                  "Automap", 556, 199)):
        controls += pair(*args)
    controls.append(obj("Image", "Image", IMAGE="controller", **at(243, 43, 268, 194)))
    controls.append(obj("Text", "Text", TEXT=HELP_TEXT, **{"WORD WRAP": True},
                        **at(-16, 237, -55, 317, ha="CENTER"), **{"SCALE WIDTH": 1}))
    return [
        obj("CEGUI Object", "CEGUI Object0", [
            obj("Window", "scrollbar", [
                obj("Window", "scrollbarwindow", [
                    obj("Image", "Image", widget="SCROLLBAR", IMAGE="scroll_button_tab_rollover",
                        VISIBLE=False, **at(0, 0, 27, 20)),
                ], **at(-1, -8, 0, -3, "CENTER", "CENTER"),
                    **{"SCALE WIDTH": 0.55, "SCALE HEIGHT": 0.9, "ALWAYS ON TOP": True}),
                obj("Image", "Image", IMAGE="sliderbar_vert_rollover", VISIBLE=False,
                    **at(0, 0, 43, 505)),
            ], VISIBLE=False, **at(0, 0, 40, -100, ha="RIGHT"), **{"SCALE HEIGHT": 1}),
            obj("Window", "ParentWindow", [
                obj("Window", "ScrollWindow", controls, widget="SCROLLWINDOW",
                    **at(23, 5, 0, 0), **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
            ], **at(0, 19, 0, -100), **{"SCALE WIDTH": 1, "SCALE HEIGHT": 1}),
            obj("Window", "bottomwind", [
                obj("Text", "scrolltext", TEXT="Scroll", VISIBLE=False,
                    **at(125, 1, 245, 19, va="CENTER")),
                obj("Image", "scrollimage", IMAGE="analog_button_left_ud", VISIBLE=False,
                    **at(79, 0, 36, 48, va="CENTER")),
                obj("Image", "Bbutton", IMAGE="b_button", **at(678, 0, 21, 21, va="CENTER")),
                obj("Text", "close", TEXT="Close", **at(705, 0, 245, 19, va="CENTER")),
            ], **at(0, 17, 17, 75, va="BOTTOM"), **{"SCALE WIDTH": 1}),
            ui_header("How To Play", 220),
        ], **{"LOOK N FEEL": "ceguiWidget/ItemText"}, **at(0, 63, 772, 588)),
        obj("Image", "Image", IMAGE="black", ALPHA=0.5, **full()),
    ]


CEGUI_LAYOUTS = (
    ("media/UI/mainmenu.uilayout", main_menu),
    ("media/UI/difficulty.uilayout", difficulty),
    # No `loading.uilayout`: on a PC pak the engine then draws the PC's own loading screen
    # (`loading.layout`, its artwork, bars and tip), which is what the pack wants there.
    ("media/UI/charactercreate.uilayout", character_create),
    ("media/UI/characterload.uilayout", character_load),
)

def pad_prompt(name, glyph_name, glyph, text_name, words, x, w, widget=None):
    """A glyph and its word, as the console's dialogs place them."""
    props = {"widget": widget} if widget else {}
    return obj("Window", name, [
        obj("Image", glyph_name, IMAGE=glyph, **at(12, 19, 21, 21)),
        obj("Text", text_name, TEXT=words, FONT="Serif14", COLOR=PROMPT,
            **at(37, 18, w - 40, 24)),
    ], **props, **at(x, 8, w, 61))


def waypoints():
    """The waypoint menu: ten destinations, the code fills and shows the ones there are."""
    buttons = [("Button9", "Text2"), ("Button10_clone10", "Text2_clone308641"),
               ("Button10_clone11", "Text2_clone308662"), ("Button10_clone13", "Text2_clone308688"),
               ("Button10_clone12", "Text2_clone308675"), ("Button10_clone15", "Text2_clone308714"),
               ("Button10_clone16", "Text2_clone308727"), ("Button10_clone17", "Text2_clone308740"),
               ("Button10_clone18", "Text2_clone308753"), ("Button10_clone14", "Text2_clone308701")]
    rows = []
    for n, (name, text_name) in enumerate(buttons, start=1):
        rows.append(obj("Button", name, [
            obj("Text", text_name, widget=f"Choice{n}", TEXT="", FONT="Serif14",
                **{"HORIZONTAL ALIGN": "CENTER", "VERTICAL ALIGN": "CENTER", "WORD WRAP": True},
                **at(0, 5, 236, 36, ha="CENTER")),
        ], widget=f"Button{n}", NORMAL="ButtonStandardRed", HOVER="ButtonStandardRedHighlight",
            CLICKED="ButtonStandardRedHighlight", **at(124, 24 + 56 * (n - 1), 252, 46)))
    return [
        obj("Window", "Window19", **full()),
        obj("CEGUI Object", "CEGUI Object3_clone13", rows + [
            ui_header("Waypoints", 160),
            obj("Window", "Window15_clone15", [
                pad_prompt("confirm_clone411986", "a button_clone411992", "a_button",
                           "Text26_clone411995", "Select", -189, 171),
                pad_prompt("Window19_clone411962", "a button_clone411971", "b_button",
                           "Text26_clone411965", "Cancel", 218, 159),
            ], **at(150, 590, 438, 74, ha="CENTER")),
        ], widget="FrameB", **{"LOOK N FEEL": "ceguiWidget/LineFrame"},
            **at(0, 66, 500, 650, ha="CENTER")),
    ]


def modal_message():
    return [
        obj("CEGUI Object", "CEGUI Object3_clone13", [
            obj("Text", "Text2", widget="Dialog", TEXT="", FONT="Serif14",
                **{"HORIZONTAL ALIGN": "CENTER", "VERTICAL ALIGN": "CENTER", "WORD WRAP": True},
                **at(0, 50, 470, 280, ha="CENTER")),
            obj("Window", "Window15_clone15", [
                pad_prompt("Window19_clone411962", "a button_clone411971", "b_button",
                           "Text26_clone411965", "Decline", 338, 159, widget="Decline"),
                pad_prompt("confirm_clone3", "a button_clone335555", "a_button",
                           "Text26_clone335558", "OK", 338, 135, widget="Ok"),
                pad_prompt("confirm_clone411986", "a button_clone411992", "a_button",
                           "Text26_clone411995", "Accept", 240, 135, widget="Accept"),
            ], **at(50, 340, 438, 74, ha="CENTER")),
            ui_header("Title", 160),
        ], widget="FrameB", **{"LOOK N FEEL": "ceguiWidget/ItemText"},
            **at(0, 126, 530, 414, ha="CENTER")),
    ]


def fishing():
    """The fishing ring, in the PC's own ring art."""
    return [
        obj("Image", "Image1_clone1", [
            obj("Image", "Image0", IMAGE="ButtonFishing", **at(0, 0, 60, 60, "CENTER", "CENTER")),
            obj("Image", "Image2_clone3", IMAGE="FishingRing", **at(0, 0, 128, 128, "CENTER",
                                                                     "CENTER")),
            obj("Image", "Image2_clone2", widget="FishingRing", IMAGE="FishingRing",
                **at(0, 0, 128, 128, "CENTER", "CENTER")),
            obj("Image", "Image5", IMAGE="a_button", **at(52, 198, 21, 21)),
            obj("Text", "Text7", TEXT="Catch", FONT="Serif14", **at(78, 197, 110, 22)),
            obj("Image", "Image6_clone6", IMAGE="b_button", **at(52, 226, 21, 21)),
            obj("Text", "Text8_clone8", TEXT="Cancel", FONT="Serif14", **at(78, 225, 110, 22)),
        ], IMAGE="FishingRipples", **at(0, 0, 200, 200, "CENTER", "CENTER")),
    ]


UI_LAYOUTS = (
    ("media/UI/options_Xenon.layout", pause_menu),
    ("media/UI/settingsmenu_xenon.layout", options),
    ("media/UI/howto_xenon.layout", help_screen),
    ("media/UI/waypointmenu_xenon.layout", waypoints),
    ("media/UI/modalmenu_xenon.layout", modal_message),
    ("media/UI/fishing_xenon.layout", fishing),
)
