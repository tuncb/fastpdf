"""Package the approved PNG into a Windows ICO. Pillow is needed only to regenerate it."""
from pathlib import Path

from PIL import Image


def main():
    assets = Path(__file__).resolve().parents[1] / "src" / "assets"
    with Image.open(assets / "fastpdf.png") as source:
        image = source.convert("RGBA")
    # Ignore faint pixels outside the mark when measuring its bounds, while
    # retaining a two-pixel border for the antialiased silhouette itself.
    bounds = image.getchannel("A").point(lambda value: 255 if value >= 128 else 0).getbbox()
    if bounds is None:
        raise ValueError("The app icon source is transparent")
    left, top, right, bottom = bounds
    icon = image.crop((max(0, left - 2), max(0, top - 2),
                       min(image.width, right + 2), min(image.height, bottom + 2)))
    edge = max(icon.size)
    padding = (edge + 13) // 14
    side = edge + 2 * padding
    canvas = Image.new("RGBA", (side, side))
    canvas.alpha_composite(icon, ((side - icon.width) // 2, (side - icon.height) // 2))
    sizes = [(size, size) for size in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)]
    destination = assets / "fastpdf.ico"
    canvas.save(destination, format="ICO", sizes=sizes)
    print(f"Wrote {destination} with {len(sizes)} icon sizes")


if __name__ == "__main__":
    main()
