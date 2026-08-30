#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import statistics
import struct
from pathlib import Path

from PIL import Image, ImageChops


OUTPUT_SIZE = (884, 450)
WORDMARK_REGION = (34, 326, 390, 438)
SKY_CROP_BIAS = 0.78

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = REPO_ROOT / "tools" / "branding" / "splash_source.png"
BMP_PATH = REPO_ROOT / "src" / "xr_3da" / "splash_oxr.bmp"
PREVIEW_PATH = REPO_ROOT / "tools" / "branding" / "splash_preview.png"


def cover_fit(source: Image.Image) -> tuple[Image.Image, tuple[int, int], tuple[int, int, int, int]]:
    output_width, output_height = OUTPUT_SIZE
    scale = max(output_width / source.width, output_height / source.height)
    resized_size = (
        max(output_width, round(source.width * scale)),
        max(output_height, round(source.height * scale)),
    )
    resized = source.resize(resized_size, Image.Resampling.LANCZOS)
    excess_x = resized.width - output_width
    excess_y = resized.height - output_height
    crop_left = excess_x // 2

    # A downward-biased crop removes extra sky while protecting the lower-left wordmark.
    crop_top = round(excess_y * SKY_CROP_BIAS) if excess_y else 0
    crop_top = max(0, min(excess_y, crop_top))
    crop_box = (crop_left, crop_top, crop_left + output_width, crop_top + output_height)
    return resized.crop(crop_box), resized_size, crop_box


def perceived_luminance(pixel: tuple[int, int, int]) -> float:
    red, green, blue = pixel
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue


def measure_wordmark_contrast(image: Image.Image) -> tuple[float, float, float, int, int]:
    pixels = list(image.crop(WORDMARK_REGION).get_flattened_data())
    foreground = [pixel for pixel in pixels if perceived_luminance(pixel) >= 50.0]
    background = [pixel for pixel in pixels if perceived_luminance(pixel) <= 20.0]
    assert len(foreground) >= 2_000, "Wordmark outline sample is unexpectedly small"
    assert len(background) >= 20_000, "Wordmark background sample is unexpectedly small"

    foreground_luma = statistics.fmean(perceived_luminance(pixel) for pixel in foreground)
    background_luma = statistics.fmean(perceived_luminance(pixel) for pixel in background)
    contrast_ratio = (foreground_luma + 0.05) / (background_luma + 0.05)
    return foreground_luma, background_luma, contrast_ratio, len(foreground), len(background)


def validate_bmp(path: Path) -> dict[str, int | str]:
    data = path.read_bytes()
    assert data[:2] == b"BM", "Expected a Windows BMP signature"
    header_file_size = struct.unpack_from("<I", data, 2)[0]
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    planes = struct.unpack_from("<H", data, 26)[0]
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    output_width, output_height = OUTPUT_SIZE
    expected_file_size = 14 + 40 + ((output_width * 3 + 3) // 4 * 4) * output_height

    assert (width, height) == OUTPUT_SIZE, "BMP must use the requested size and bottom-up orientation"
    assert planes == 1
    assert bits_per_pixel == 24
    assert compression == 0
    assert dib_header_size == 40, "BITMAPINFOHEADER excludes embedded colour profiles"
    assert pixel_offset == 54
    assert header_file_size == len(data) == expected_file_size

    with Image.open(path) as reopened:
        reopened.load()
        assert reopened.size == OUTPUT_SIZE
        assert reopened.mode == "RGB"
        assert reopened.format == "BMP"
        assert "icc_profile" not in reopened.info

    return {
        "width": width,
        "height": height,
        "bits_per_pixel": bits_per_pixel,
        "compression": compression,
        "file_size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> None:
    with Image.open(SOURCE_PATH) as source_file:
        source_file.load()
        source_format = source_file.format
        source_size = source_file.size
        source = source_file.convert("RGB")

    converted, resized_size, crop_box = cover_fit(source)
    assert converted.size == OUTPUT_SIZE
    assert converted.mode == "RGB"

    BMP_PATH.parent.mkdir(parents=True, exist_ok=True)
    converted.save(BMP_PATH, format="BMP")
    converted.save(PREVIEW_PATH, format="PNG", optimize=False, compress_level=9)

    metadata = validate_bmp(BMP_PATH)
    with Image.open(BMP_PATH) as reopened_bmp, Image.open(PREVIEW_PATH) as reopened_preview:
        assert ImageChops.difference(
            reopened_bmp.convert("RGB"),
            reopened_preview.convert("RGB"),
        ).getbbox() is None

    foreground_luma, background_luma, contrast_ratio, foreground_count, background_count = (
        measure_wordmark_contrast(converted)
    )
    assert contrast_ratio >= 7.0

    print(f"Source: {SOURCE_PATH}")
    print(f"Source format: {source_format}; size: {source_size[0]}x{source_size[1]}; mode after load: RGB")
    print(f"Cover resize: {resized_size[0]}x{resized_size[1]}; crop box: {crop_box}")
    print(f"BMP: {BMP_PATH}")
    print(f"Preview: {PREVIEW_PATH}")
    print(f"Size: {metadata['width']}x{metadata['height']}")
    print(f"Mode: RGB; format: BMP; bit depth: {metadata['bits_per_pixel']} bpp")
    print(f"Compression: BI_RGB ({metadata['compression']}); file size: {metadata['file_size']} bytes")
    print(f"SHA-256: {metadata['sha256']}")
    print(
        f"Wordmark luminance: {foreground_luma:.2f}/255 from {foreground_count} pixels; "
        f"background: {background_luma:.2f}/255 from {background_count} pixels; "
        f"contrast ratio: {contrast_ratio:.2f}:1"
    )


if __name__ == "__main__":
    main()
