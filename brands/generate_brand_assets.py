#!/usr/bin/env python3
"""
Generate SecuraCV brand assets for HACS from a source canary image.

Usage:
    python3 generate_brand_assets.py <source_image>

This script generates:
    - icon.png (256x256) - Standard resolution icon
    - icon@2x.png (512x512) - High DPI icon
    - logo.png (512x180) - Standard resolution logo
    - logo@2x.png (1024x360) - High DPI logo
"""

import sys
from pathlib import Path
from PIL import Image

def generate_icon(source: Image.Image, size: int, output_path: Path) -> None:
    """Generate a square icon, centered with transparent background."""
    # Create a new transparent image
    icon = Image.new('RGBA', (size, size), (0, 0, 0, 0))

    # Resize source to fit within the icon, maintaining aspect ratio
    source_ratio = source.width / source.height
    if source_ratio > 1:
        # Wider than tall
        new_width = size
        new_height = int(size / source_ratio)
    else:
        # Taller than wide or square
        new_height = size
        new_width = int(size * source_ratio)

    # Use high-quality resampling
    resized = source.resize((new_width, new_height), Image.Resampling.LANCZOS)

    # Center the image
    x = (size - new_width) // 2
    y = (size - new_height) // 2
    icon.paste(resized, (x, y), resized if resized.mode == 'RGBA' else None)

    icon.save(output_path, 'PNG')
    print(f"Generated: {output_path} ({size}x{size})")


def generate_logo(source: Image.Image, width: int, height: int, output_path: Path) -> None:
    """Generate a wide logo with the canary centered."""
    # Create a new transparent image
    logo = Image.new('RGBA', (width, height), (0, 0, 0, 0))

    # For logo, we want the canary to be prominent but fit the height
    # Scale to fit height while maintaining aspect ratio
    source_ratio = source.width / source.height
    new_height = int(height * 0.95)  # 95% of height for some padding
    new_width = int(new_height * source_ratio)

    # If too wide, scale by width instead
    if new_width > width * 0.8:
        new_width = int(width * 0.8)
        new_height = int(new_width / source_ratio)

    # Use high-quality resampling
    resized = source.resize((new_width, new_height), Image.Resampling.LANCZOS)

    # Center horizontally and vertically
    x = (width - new_width) // 2
    y = (height - new_height) // 2
    logo.paste(resized, (x, y), resized if resized.mode == 'RGBA' else None)

    logo.save(output_path, 'PNG')
    print(f"Generated: {output_path} ({width}x{height})")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_brand_assets.py <source_image>")
        print("\nThis script generates HACS brand assets from a source canary image.")
        sys.exit(1)

    source_path = Path(sys.argv[1])
    if not source_path.exists():
        print(f"Error: Source image not found: {source_path}")
        sys.exit(1)

    # Load source image
    source = Image.open(source_path)
    if source.mode != 'RGBA':
        source = source.convert('RGBA')

    print(f"Source image: {source_path} ({source.width}x{source.height})")

    # Output directory (same as this script)
    output_dir = Path(__file__).parent

    # Generate icons
    generate_icon(source, 256, output_dir / 'icon.png')
    generate_icon(source, 512, output_dir / 'icon@2x.png')

    # Generate logos (wider format)
    generate_logo(source, 512, 180, output_dir / 'logo.png')
    generate_logo(source, 1024, 360, output_dir / 'logo@2x.png')

    print("\nAll brand assets generated successfully!")
    print("\nNext steps:")
    print("1. Review the generated images")
    print("2. Update SUBMIT_TO_BRANDS.md if needed")
    print("3. Commit and push the changes")


if __name__ == '__main__':
    main()
