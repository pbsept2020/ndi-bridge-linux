#!/usr/bin/env python3
"""
Generate NDI Test Pattern app icon — dark gray squircle style
matching the NDI Viewer icon aesthetic.

Outputs: icon_512.png, icon_256.png, icon_128.png + AppIcon.icns
"""

import os
import sys
import subprocess
from PIL import Image, ImageDraw, ImageFont

def rounded_rect_mask(size, radius):
    """Create a mask with rounded corners (squircle approximation)."""
    mask = Image.new('L', size, 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([0, 0, size[0]-1, size[1]-1], radius=radius, fill=255)
    return mask

def draw_icon(size):
    """Draw the NDI PATTERN icon at given size."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Scale factor relative to 512
    s = size / 512.0

    # Background: dark gray squircle
    radius = int(90 * s)
    margin = int(8 * s)
    bg_color = (68, 68, 72, 255)  # #444448 — dark gray with slight cool tint
    draw.rounded_rectangle(
        [margin, margin, size - margin - 1, size - margin - 1],
        radius=radius,
        fill=bg_color
    )

    # Subtle inner border (slightly lighter)
    border_color = (82, 82, 86, 255)
    draw.rounded_rectangle(
        [margin, margin, size - margin - 1, size - margin - 1],
        radius=radius,
        outline=border_color,
        width=max(1, int(1.5 * s))
    )

    # Load fonts
    sz_bold = max(6, int(120 * s))
    sz_sub  = max(4, int(42 * s))
    sz_tc   = max(4, int(28 * s))

    try:
        font_bold = ImageFont.truetype('/System/Library/Fonts/HelveticaNeue.ttc', sz_bold, index=8)
    except Exception:
        try:
            font_bold = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', sz_bold, index=1)
        except Exception:
            font_bold = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', sz_bold)

    try:
        font_sub = ImageFont.truetype('/System/Library/Fonts/HelveticaNeue.ttc', sz_sub, index=4)
    except Exception:
        font_sub = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', sz_sub)

    try:
        font_tc = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', sz_tc)
    except Exception:
        font_tc = ImageFont.truetype('/System/Library/Fonts/Courier.dfont', sz_tc)

    center_x = size // 2

    # "NDI" text — white, bold, centered
    ndi_text = "NDI"
    ndi_bbox = draw.textbbox((0, 0), ndi_text, font=font_bold)
    ndi_w = ndi_bbox[2] - ndi_bbox[0]
    ndi_h = ndi_bbox[3] - ndi_bbox[1]
    ndi_x = center_x - ndi_w // 2
    ndi_y = int(130 * s)

    draw.text((ndi_x, ndi_y), ndi_text, fill=(255, 255, 255, 255), font=font_bold)

    # Orange dot — upper right of "I", like NDI branding
    dot_r = int(8 * s)
    dot_x = ndi_x + ndi_w + int(6 * s)
    dot_y = ndi_y + int(10 * s)
    draw.ellipse(
        [dot_x - dot_r, dot_y - dot_r, dot_x + dot_r, dot_y + dot_r],
        fill=(255, 140, 0, 255)  # #FF8C00 orange matching TC display
    )

    # "PATTERN" text — orange, smaller, centered below NDI
    sub_text = "PATTERN"
    sub_bbox = draw.textbbox((0, 0), sub_text, font=font_sub)
    sub_w = sub_bbox[2] - sub_bbox[0]
    sub_x = center_x - sub_w // 2
    sub_y = ndi_y + ndi_h + int(12 * s)

    draw.text((sub_x, sub_y), sub_text, fill=(255, 140, 0, 220), font=font_sub)

    # Small timecode preview — subtle, below PATTERN
    tc_text = "12:34:56:07"
    tc_bbox = draw.textbbox((0, 0), tc_text, font=font_tc)
    tc_w = tc_bbox[2] - tc_bbox[0]
    tc_x = center_x - tc_w // 2
    tc_y = sub_y + int(52 * s)

    # Tiny dark panel behind TC
    panel_pad_x = int(10 * s)
    panel_pad_y = int(4 * s)
    panel_r = int(6 * s)
    draw.rounded_rectangle(
        [tc_x - panel_pad_x, tc_y - panel_pad_y,
         tc_x + tc_w + panel_pad_x, tc_y + (tc_bbox[3] - tc_bbox[1]) + panel_pad_y],
        radius=panel_r,
        fill=(20, 20, 20, 200)
    )
    draw.text((tc_x, tc_y), tc_text, fill=(255, 140, 0, 140), font=font_tc)

    return img

def make_icns(output_dir):
    """Generate .icns from PNG files using iconutil."""
    iconset = os.path.join(output_dir, 'AppIcon.iconset')
    os.makedirs(iconset, exist_ok=True)

    sizes = [16, 32, 64, 128, 256, 512]
    for sz in sizes:
        img = draw_icon(sz)
        img.save(os.path.join(iconset, f'icon_{sz}x{sz}.png'))
        # @2x version
        img2x = draw_icon(sz * 2)
        img2x.save(os.path.join(iconset, f'icon_{sz}x{sz}@2x.png'))

    icns_path = os.path.join(output_dir, 'AppIcon.icns')
    subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', icns_path], check=True)

    # Clean up iconset
    import shutil
    shutil.rmtree(iconset)

    return icns_path

if __name__ == '__main__':
    output_dir = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) > 1:
        output_dir = sys.argv[1]

    os.makedirs(output_dir, exist_ok=True)

    # Preview PNG
    preview = draw_icon(512)
    preview_path = os.path.join(output_dir, 'icon_512.png')
    preview.save(preview_path)
    print(f"Preview: {preview_path}")

    # .icns
    icns = make_icns(output_dir)
    print(f"Icon: {icns}")
