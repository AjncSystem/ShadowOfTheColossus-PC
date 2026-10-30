"""Render size patches (sotc_modes.py --render): the display object's aspect above 16:9."""
from paths import ROOT  # noqa: F401
import os
import struct
import unittest
from unittest.mock import patch
import sotc_modes

ORIGINALS = {address: bytes.fromhex(original)
             for address, _, original in [*sotc_modes.RENDER_SITES, sotc_modes.WIDE_ASPECT]}


def fake_image_bytes(elf, ph, vaddr, length):
    """The original bytes at every render size site, as in the EU 1.01 / US 1.00 eboot."""
    return ORIGINALS[vaddr + sotc_modes.BASE][:length]


def aspect_write(size, env):
    """The float written over the display object's aspect for size, None when it is not."""
    address, _, original = sotc_modes.WIDE_ASPECT
    at = address - sotc_modes.BASE + len(bytes.fromhex(original)) - 4
    clean = {k: v for k, v in os.environ.items() if not k.startswith('SOTC_')}
    with patch.object(sotc_modes, 'image_bytes', fake_image_bytes), patch.dict(os.environ, {**clean, **env}, clear=True):
        writes = dict(sotc_modes.render_patches(None, None, size))
    return struct.unpack('<f', writes[at])[0] if at in writes else None


class WideAspectTest(unittest.TestCase):
    def test_wide_renders_patch_the_display_aspect(self):
        # Left at 16:9 the 3D scene was stretched sideways (#327, #331, #341).
        for size in ('2560x1080', '3440x1440', '3840x1600', '5120x1440', '1704x720'):
            w, h = map(int, size.split('x'))
            self.assertAlmostEqual(aspect_write(size, {}), w / h, places=5, msg=size)

    def test_16_9_and_narrower_keep_it(self):
        for size in ('2560x1440', '3840x2160', '1920x1200', '1704x960'):
            self.assertIsNone(aspect_write(size, {}), size)

    def test_switch(self):
        self.assertIsNone(aspect_write('2560x1080', {'SOTC_WIDE_ASPECT': '0'}))
        self.assertAlmostEqual(aspect_write('1920x1200', {'SOTC_WIDE_ASPECT': '1'}), 1.6, places=5)


if __name__ == '__main__':
    unittest.main()
