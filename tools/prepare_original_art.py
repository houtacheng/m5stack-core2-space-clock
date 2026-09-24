from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "SpaceClockNative" / "art-source"
OUTPUT = ROOT / "SpaceClockNative" / "art"
OUTPUT.mkdir(parents=True, exist_ok=True)


def fit(source: Image.Image, box: tuple[int, int], inset: int = 2) -> Image.Image:
    alpha = source.getchannel("A")
    bounds = alpha.getbbox()
    if bounds:
        source = source.crop(bounds)
    target = Image.new("RGBA", box, (0, 0, 0, 0))
    source.thumbnail((box[0] - inset * 2, box[1] - inset * 2), Image.Resampling.LANCZOS)
    target.alpha_composite(source, ((box[0] - source.width) // 2, (box[1] - source.height) // 2))
    return target


sheet = Image.open(SOURCE / "space-characters.png").convert("RGBA")
w, h = sheet.size
quadrants = {
    "cosmonaut_0.png": (0, 0, w // 2, h // 2),
    "cosmonaut_1.png": (w // 2, 0, w, h // 2),
    "satellite_0.png": (0, h // 2, w // 2, h),
    "satellite_1.png": (w // 2, h // 2, w, h),
}
for name, crop in quadrants.items():
    size = (105, 130) if name.startswith("cosmonaut") else (132, 85)
    fit(sheet.crop(crop), size).save(OUTPUT / name, optimize=True)

planet = Image.open(SOURCE / "space-planet.png").convert("RGBA")
fit(planet, (200, 112), 0).save(OUTPUT / "background.png", optimize=True)

# Normalize navigation artwork by visible alpha bounds, so unlike source
# canvases have the same apparent size. Clock navigation shows two 20 px icons
# per virtual button: short-press action first, long-press action second.
for source_name, output_name in (
    ("companion-nav-source.png", "nav_companion.png"),
    ("zen-nav-source.png", "nav_meditation.png"),
    ("emotion-nav-source.png", "nav_emotion.png"),
    ("hass-nav-source.png", "nav_hass.png"),
    ("nightlight-nav-source.png", "nav_nightlight.png"),
    ("settings-nav-source.png", "nav_settings.png"),
):
    source_path = SOURCE / source_name
    if source_path.exists():
        fit(Image.open(source_path).convert("RGBA"), (20, 20), 1).save(OUTPUT / output_name, optimize=True)

# Shared bottom-bar actions. Keeping these in one normalized 20 px system
# makes their apparent size consistent even though the supplied source art has
# very different transparent margins and silhouettes.
for source_name, output_name in (
    ("action-close-source.png", "action_close.png"),
    ("action-install-source.png", "action_install.png"),
    ("action-check-source.png", "action_check.png"),
    ("action-next-source.png", "action_next.png"),
    ("action-previous-source.png", "action_previous.png"),
    ("action-clock-source.png", "action_clock.png"),
):
    source_path = SOURCE / source_name
    if source_path.exists():
        fit(Image.open(source_path).convert("RGBA"), (20, 20), 1).save(OUTPUT / output_name, optimize=True)
