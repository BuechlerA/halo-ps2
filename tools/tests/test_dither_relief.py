"""ADR-066: sky textures are quantised at full colour precision with error diffusion (no bands in a dark gradient), and a bump height field bakes into a colour map as relief shading."""
import sys
from collections import Counter
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import texconv  # noqa: E402


def gradient(w: int, h: int, lo: int, hi: int) -> bytes:
    """A dark horizontal ramp, opaque RGBA."""
    out = bytearray()
    for _y in range(h):
        for x in range(w):
            v = lo + (hi - lo) * x // (w - 1)
            out += bytes((v, v, v + 8, 255))
    return bytes(out)


def test_full_precision_keys():
    assert texconv._expand(0x00112233, True) == (0x33, 0x22, 0x11, 255)
    assert texconv._expand(0x00F8FCF8)[:3] == (255, 255, 255)           # the reduced path still replicates bits


def test_smooth_conversion_beats_565_banding():
    w, h = 64, 8
    img = gradient(w, h, 10, 40)                                         # 30 levels over 64 texels: 565 reduction folds this into a handful of steps
    plain = texconv.convert_rgba(1, img, w, h, texconv.CLASS_OPAQUE, 8)
    smooth = texconv.convert_rgba(1, img, w, h, texconv.CLASS_OPAQUE, 8, smooth=True)
    assert smooth.psm == plain.psm == texconv.PSMT8
    assert smooth.colours >= plain.colours                               # full precision keeps more distinct dark levels
    # the mean of each 8-texel column group follows the ramp more closely with diffusion than the nearest-entry mapping of the reduced histogram
    assert smooth.rms <= plain.rms + 1.0


def test_dither_is_deterministic_and_in_range():
    w, h = 32, 16
    img = gradient(w, h, 5, 60)
    hist = Counter(texconv._to_px(img))
    q = texconv.Quantiser(hist, 16, texconv.MASK_RGB8)
    a, b = q.dithered(img, w, h), q.dithered(img, w, h)
    assert a == b and len(a) == w * h and max(a) < 16
    row = a[:w]
    assert len(set(row)) > 3                                             # the ramp uses several entries along a row


def test_relief_shading():
    w = h = 8
    rgba = bytes((100, 100, 100, 255) * (w * h))
    flat = bytes([128] * (w * h))
    assert texconv.bake_relief(rgba, w, h, flat)[0] == round(100 * texconv.BUMP_GAIN)    # a flat height field only applies the gain
    rgba_g = texconv.bake_relief(rgba, w, h, flat)
    height = bytearray([128] * (w * h))
    for y in range(h):
        for x in range(4, w):
            height[y * w + x] = 200                                      # a step: raised on the right
    out = texconv.bake_relief(rgba, w, h, bytes(height))
    px = lambda x, y: out[4 * (y * w + x)]
    assert px(3, 4) < rgba_g[0] and px(4, 4) < rgba_g[0]                              # a rise away from the up-left light darkens the texels either side of the edge
    rev = bytes(height[y * w + (w - 1 - x)] for y in range(h) for x in range(w))
    back = texconv.bake_relief(rgba, w, h, rev)
    assert back[4 * (4 * w + 3)] > rgba_g[0] and back[4 * (4 * w + 4)] > rgba_g[0]    # and a drop towards it brightens them
    assert px(1, 4) == rgba_g[0] and px(6, 4) == rgba_g[0]               # away from the edge only the gain applies
    assert out[3::4] == rgba[3::4]                                       # alpha untouched


if __name__ == "__main__":
    test_full_precision_keys()
    test_smooth_conversion_beats_565_banding()
    test_dither_is_deterministic_and_in_range()
    test_relief_shading()
    print("ok")
