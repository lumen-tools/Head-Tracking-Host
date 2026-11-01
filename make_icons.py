"""Creates icon.ico (Windows) and icon.icns (macOS) for the build."""
from app import make_icon_image

img = make_icon_image(1024)
img.save("icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.save("icon.icns")
print("created icon.ico and icon.icns")
