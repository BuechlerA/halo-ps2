"""SKY0 section (docs/13-scen-section.md §Sky): layout with the second sky's model tag, and the pseudo shader ids of sky layers."""
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import objtex, skyconv  # noqa: E402


def test_build_section() -> None:
    sky = {"path": "sky\\space\\space", "model_tag": 0xE1760002, "model_tag2": 0xE183000F, "fog_rgb": (0.1, 0.2, 0.3),
           "fog_density": 1.0, "fog_near": 1.0, "fog_far": 120.0, "outdoor": (0.5, 0.6, 0.7), "indoor": (0.25, 0.26, 0.65)}
    real = skyconv.read_sky
    skyconv.read_sky = lambda path: sky
    try:
        blob = skyconv.build_section(Path("x.map"))
    finally:
        skyconv.read_sky = real
    assert len(blob) == skyconv.SECTION_SIZE == 64
    assert blob[:4] == b"SKYS" and struct.unpack_from("<HHI", blob, 4) == (1, 0, 0xE1760002)
    assert struct.unpack_from("<3f", blob, 12) == struct.unpack("<3f", struct.pack("<3f", 0.1, 0.2, 0.3))
    assert struct.unpack_from("<I", blob, 60)[0] == 0xE183000F            # the second sky: the engine reads it at +60
    no_sky = None
    skyconv.read_sky = lambda path: no_sky
    try:
        empty = skyconv.build_section(Path("x.map"))
    finally:
        skyconv.read_sky = real
    assert len(empty) == 64 and struct.unpack_from("<I", empty, 8)[0] == 0 and struct.unpack_from("<I", empty, 60)[0] == 0


def test_layer_ids() -> None:
    a = objtex.layer_shader_id(0xE1840010, 1)
    b = objtex.layer_shader_id(0xE1840010, 2)
    assert a != b and a >= objtex.LAYER_BASE and objtex.layer_shader_id(0xE1840010, 1) == a      # stable per (shader, map)


if __name__ == "__main__":
    test_build_section()
    test_layer_ids()
    print("ok")
