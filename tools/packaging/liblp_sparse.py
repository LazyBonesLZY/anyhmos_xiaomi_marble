#!/usr/bin/env python3
"""Pure-Python LibLP v10 + Android Sparse super.img builder and verifier.

Constructs an exact-capacity Android sparse dynamic-partition super image whose
LibLP v10 geometry, primary/backup metadata slots, extents, and group bounds
pass both tools/packaging/android_sparse.py:inspect_super() and
compat/bootstrap/hmos_probe_v121.c:partitions().
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
from typing import Iterable

from android_sparse import SparseImage, inspect_super


SECTOR_SIZE = 512
SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_DONT_CARE = 0xCAC3

LP_GEOMETRY_MAGIC = 0x616C4467
LP_GEOMETRY_SIZE = 52
LP_HEADER_MAGIC = 0x414C5030
LP_HEADER_MAJOR = 10
LP_HEADER_MINOR = 0
LP_HEADER_SIZE = 128
LP_PARTITION_ATTR_READONLY = 0x1
LP_TARGET_TYPE_LINEAR = 0


def sha256_file(path: Path, chunk_size: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            buf = stream.read(chunk_size)
            if not buf:
                break
            digest.update(buf)
    return digest.hexdigest()


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_lp_geometry(
    metadata_max_size: int = 65536,
    metadata_slot_count: int = 3,
    logical_block_size: int = 4096,
) -> bytes:
    """Build a 4096-byte LpMetadataGeometry block with valid SHA-256 checksum."""
    raw = bytearray(LP_GEOMETRY_SIZE)
    struct.pack_into('<II', raw, 0, LP_GEOMETRY_MAGIC, LP_GEOMETRY_SIZE)
    struct.pack_into('<III', raw, 40, metadata_max_size, metadata_slot_count, logical_block_size)
    raw[8:40] = hashlib.sha256(raw).digest()
    return bytes(raw) + bytes(4096 - LP_GEOMETRY_SIZE)


def build_lp_metadata(
    super_bytes: int,
    slot_a_images: list[tuple[str, Path]],
    *,
    metadata_max_size: int = 65536,
    metadata_slot_count: int = 3,
    logical_block_size: int = 4096,
    alignment: int = 1048576,
    group_prefix: str = 'qti_dynamic_partitions',
    include_empty_b_slots: bool = True,
) -> tuple[bytes, list[dict]]:
    """Build one padded LibLP v10 metadata slot and return (slot_bytes, placements).

    placements contains dicts: {name, path, start_offset, raw_size, padded_size}.
    """
    if super_bytes % logical_block_size != 0 or super_bytes <= alignment:
        raise ValueError(f'Invalid super_bytes: {super_bytes}')
    if alignment % logical_block_size != 0:
        raise ValueError(f'alignment ({alignment}) must be a multiple of logical_block_size ({logical_block_size})')

    metadata_region_end = 12288 + 2 * metadata_slot_count * metadata_max_size
    first_offset = align_up(metadata_region_end, alignment)
    first_logical_sector = first_offset // SECTOR_SIZE

    groups = [
        ('default', 0, 0),
        (f'{group_prefix}_a', 0, super_bytes),
    ]
    if include_empty_b_slots:
        groups.append((f'{group_prefix}_b', 0, super_bytes))

    partitions = []
    extents = []
    placements = []
    cursor = first_offset

    for part_name, img_path in slot_a_images:
        raw_size = img_path.stat().st_size
        if raw_size <= 0:
            raise ValueError(f'Partition image {part_name} ({img_path}) is empty')
        padded_size = align_up(raw_size, logical_block_size)
        cursor = align_up(cursor, alignment)
        if cursor + padded_size > super_bytes:
            raise ValueError(
                f'Partition {part_name} ({padded_size} bytes) overflows super ({super_bytes} bytes) at offset {cursor}'
            )
        start_sector = cursor // SECTOR_SIZE
        num_sectors = padded_size // SECTOR_SIZE
        first_ext_idx = len(extents)
        extents.append((num_sectors, LP_TARGET_TYPE_LINEAR, start_sector, 0))
        partitions.append((f'{part_name}_a', LP_PARTITION_ATTR_READONLY, first_ext_idx, 1, 1))
        placements.append(
            {
                'name': f'{part_name}_a',
                'logical_name': part_name,
                'path': img_path,
                'start_offset': cursor,
                'raw_size': raw_size,
                'padded_size': padded_size,
                'start_sector': start_sector,
                'num_sectors': num_sectors,
            }
        )
        cursor += padded_size

    if include_empty_b_slots:
        for part_name, _ in slot_a_images:
            partitions.append((f'{part_name}_b', LP_PARTITION_ATTR_READONLY, 0, 0, 2))

    # Serialize tables: partitions (52 B), extents (24 B), groups (48 B), block_devices (64 B)
    part_table = bytearray()
    for name, attrs, first_ext, num_ext, grp_idx in partitions:
        name_b = name.encode('ascii')
        if len(name_b) > 36:
            raise ValueError(f'Partition name too long: {name}')
        part_table.extend(struct.pack('<36sIIII', name_b, attrs, first_ext, num_ext, grp_idx))

    ext_table = bytearray()
    for num_sectors, target_type, target_data, target_source in extents:
        ext_table.extend(struct.pack('<QIQI', num_sectors, target_type, target_data, target_source))

    grp_table = bytearray()
    for name, flags, max_size in groups:
        grp_table.extend(struct.pack('<36sIQ', name.encode('ascii'), flags, max_size))

    dev_table = struct.pack(
        '<QIIQ36sI',
        first_logical_sector,
        alignment,
        0,
        super_bytes,
        b'super',
        0,
    )

    off_part = 0
    off_ext = off_part + len(part_table)
    off_grp = off_ext + len(ext_table)
    off_dev = off_grp + len(grp_table)
    tables = bytes(part_table + ext_table + grp_table + dev_table)

    if LP_HEADER_SIZE + len(tables) > metadata_max_size:
        raise ValueError('LibLP metadata exceeds metadata_max_size')

    header = bytearray(LP_HEADER_SIZE)
    struct.pack_into('<IHHI', header, 0, LP_HEADER_MAGIC, LP_HEADER_MAJOR, LP_HEADER_MINOR, LP_HEADER_SIZE)
    struct.pack_into('<I', header, 44, len(tables))
    header[48:80] = hashlib.sha256(tables).digest()
    struct.pack_into('<III', header, 80, off_part, len(partitions), 52)
    struct.pack_into('<III', header, 92, off_ext, len(extents), 24)
    struct.pack_into('<III', header, 104, off_grp, len(groups), 48)
    struct.pack_into('<III', header, 116, off_dev, 1, 64)
    header[12:44] = hashlib.sha256(header).digest()

    slot_bytes = bytes(header) + tables + bytes(metadata_max_size - LP_HEADER_SIZE - len(tables))
    return slot_bytes, placements


def build_super_sparse_image(
    output_path: Path,
    super_bytes: int,
    slot_a_images: Iterable[tuple[str, Path]],
    *,
    metadata_max_size: int = 65536,
    metadata_slot_count: int = 3,
    logical_block_size: int = 4096,
    alignment: int = 1048576,
    max_raw_chunk_bytes: int = 64 * 1024 * 1024,
    verify_readback: bool = True,
) -> dict:
    """Write an Android sparse LibLP v10 super.img and verify layout & SHA-256 readback."""
    images = [(name, Path(path)) for name, path in slot_a_images]
    if max_raw_chunk_bytes % logical_block_size != 0 or max_raw_chunk_bytes <= 0:
        raise ValueError('max_raw_chunk_bytes must be a positive multiple of logical_block_size')

    geometry = build_lp_geometry(metadata_max_size, metadata_slot_count, logical_block_size)
    metadata_slot, placements = build_lp_metadata(
        super_bytes,
        images,
        metadata_max_size=metadata_max_size,
        metadata_slot_count=metadata_slot_count,
        logical_block_size=logical_block_size,
        alignment=alignment,
    )

    # Leading region: 4096 reserved zero + primary geometry + backup geometry + 2*slots metadata
    prefix = bytes(4096) + geometry + geometry + metadata_slot * (metadata_slot_count * 2)
    if len(prefix) % logical_block_size != 0:
        prefix += bytes(logical_block_size - (len(prefix) % logical_block_size))

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + '.tmp')

    total_blocks = super_bytes // logical_block_size
    chunk_count = 0
    logical_cursor = 0

    def emit_raw_bytes(out_f, data: bytes) -> None:
        nonlocal chunk_count, logical_cursor
        assert len(data) % logical_block_size == 0
        nblocks = len(data) // logical_block_size
        out_f.write(struct.pack('<HHII', CHUNK_RAW, 0, nblocks, 12 + len(data)))
        out_f.write(data)
        chunk_count += 1
        logical_cursor += len(data)

    def emit_dont_care(out_f, nbytes: int) -> None:
        nonlocal chunk_count, logical_cursor
        if nbytes == 0:
            return
        assert nbytes % logical_block_size == 0
        # Split giant DONT_CARE ranges cleanly if ever > 2^31 blocks (safe for any size)
        rem_blocks = nbytes // logical_block_size
        while rem_blocks > 0:
            take = min(rem_blocks, 0x10000000)
            out_f.write(struct.pack('<HHII', CHUNK_DONT_CARE, 0, take, 12))
            chunk_count += 1
            logical_cursor += take * logical_block_size
            rem_blocks -= take

    with tmp_path.open('wb') as out_f:
        # Reserve 28-byte Android sparse header
        out_f.write(bytes(28))
        emit_raw_bytes(out_f, prefix)

        for item in placements:
            gap = item['start_offset'] - logical_cursor
            if gap < 0:
                raise ValueError(f'Overlapping placement for {item["name"]}')
            emit_dont_care(out_f, gap)

            remaining = item['padded_size']
            raw_rem = item['raw_size']
            with item['path'].open('rb') as src_f:
                while remaining > 0:
                    take = min(remaining, max_raw_chunk_bytes)
                    nblocks = take // logical_block_size
                    out_f.write(struct.pack('<HHII', CHUNK_RAW, 0, nblocks, 12 + take))
                    to_copy = min(take, raw_rem)
                    copied = 0
                    while copied < to_copy:
                        buf = src_f.read(min(4 * 1024 * 1024, to_copy - copied))
                        if not buf:
                            raise IOError(f'Unexpected EOF reading {item["path"]}')
                        out_f.write(buf)
                        copied += len(buf)
                    if take > to_copy:
                        out_f.write(bytes(take - to_copy))
                    raw_rem -= to_copy
                    remaining -= take
                    chunk_count += 1
                    logical_cursor += take

        emit_dont_care(out_f, super_bytes - logical_cursor)
        assert logical_cursor == super_bytes

        # Patch Android sparse header at offset 0
        out_f.seek(0)
        out_f.write(
            struct.pack(
                '<IHHHHIIII',
                SPARSE_MAGIC,
                1,
                0,
                28,
                12,
                logical_block_size,
                total_blocks,
                chunk_count,
                0,
            )
        )

    tmp_path.replace(output_path)

    sparse = SparseImage(output_path)
    try:
        layout = inspect_super(sparse)
        if sparse.size != super_bytes or layout['devices'][0]['bytes'] != super_bytes:
            raise ValueError('Super expanded capacity mismatch after write')
        if verify_readback:
            by_name = {item['name']: item for item in placements}
            for part in layout['partitions']:
                if part['name'].endswith('_b'):
                    if part['bytes'] != 0:
                        raise ValueError(f'Expected empty B slot for {part["name"]}')
                    continue
                item = by_name[part['name']]
                start = part['extents'][0][2] * SECTOR_SIZE
                expected_sha = sha256_file(item['path'])
                actual_sha = sparse.hash_range(start, item['raw_size'])
                if actual_sha != expected_sha:
                    raise ValueError(f'Readback SHA mismatch for {part["name"]}')
                part['content_sha256'] = actual_sha
                part['raw_bytes'] = item['raw_size']
                part['content_readback_verified'] = True
    finally:
        sparse.close()

    layout['sparse_file_bytes'] = output_path.stat().st_size
    layout['sparse_chunks'] = chunk_count
    layout['output_path'] = str(output_path)
    return layout
