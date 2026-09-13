#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Print the root directory mode stored in a UBI-contained UBIFS image."""

import sys

from ubireader.ubi import ubi
from ubireader.ubifs import ubifs, walk
from ubireader.ubi_io import leb_virtual_file, ubi_file
from ubireader.utils import guess_peb_size, guess_start_offset


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} UBI_IMAGE", file=sys.stderr)
        return 2

    path = sys.argv[1]
    start = guess_start_offset(path)
    block_size = guess_peb_size(path)
    image_file = ubi_file(path, block_size, start, None)
    try:
        image = ubi(image_file)
        volumes = []
        for image_description in image.images:
            for volume_description in image_description.volumes.values():
                blocks = volume_description.get_blocks(image.blocks)
                if blocks:
                    volumes.append(blocks)
        if len(volumes) != 1:
            raise RuntimeError(f"expected one populated UBI volume, found {len(volumes)}")

        filesystem = ubifs(leb_virtual_file(image, volumes[0]))
        inodes = {}
        bad_blocks = []
        walk.index(filesystem, filesystem.master_node.root_lnum,
                   filesystem.master_node.root_offs, inodes, bad_blocks)
        if bad_blocks:
            raise RuntimeError(f"bad UBIFS blocks: {bad_blocks}")
        if 1 not in inodes or "ino" not in inodes[1]:
            raise RuntimeError("UBIFS root inode was not found")
        print(f"{inodes[1]['ino'].mode & 0o7777:o}")
        return 0
    finally:
        image_file.close()


if __name__ == "__main__":
    raise SystemExit(main())
