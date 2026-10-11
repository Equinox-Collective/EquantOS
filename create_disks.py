import struct
import os
import shutil
import subprocess
import sys
import zlib
import uuid

CACHE_SEED_FILES = [
    # (host source, image destination dir, image file name);
    # "CACHE:<name>" resolves against the cache dir, "RES:<name>" against res/.
    ("CACHE:FONTATLAS.BIN", "::/CACHE", "FONTATLAS.BIN"),
    ("CACHE:PRELOAD.TXT", "::/CACHE", "PRELOAD.TXT"),
    ("RES:font.psf", "::/FONTS", "FONT.PSF"),
    ("RES:examples/hello.c", "::/EXAMPLES", "HELLO.C"),
]


# Repo root = directory containing this script (so seeding works from any CWD).
REPO_ROOT = os.path.dirname(os.path.abspath(__file__))


def _host_source(token, cache_dir):
    if token.startswith("CACHE:"):
        path = os.path.join(cache_dir, token[len("CACHE:"):])
    elif token.startswith("RES:"):
        path = os.path.join(REPO_ROOT, "res", token[len("RES:"):])
    else:
        path = token
    if not os.path.isabs(path):
        path = os.path.join(REPO_ROOT, path)
    return path

# Byte offset of the FAT32 partition inside each image (LBA 2048 * 512).
MBR_FAT32_PART_OFFSET = 2048 * 512
ESP_PART_OFFSET = 2048 * 512


def _mtools_available():
    return shutil.which("mcopy") is not None and shutil.which("mmd") is not None


def seed_fat32_partition(image, part_offset, cache_dir="build/cache"):
    """Copy font + cache files into a FAT32 partition inside a raw image.

    Uses mtools (mcopy/mmd with the -i <img>@@<off> syntax), so the host
    needs no root privileges and existing image data is preserved.
    Returns True on success, False when skipped/failed (non-fatal).
    """
    if not _mtools_available():
        print("[CACHE] mtools (mcopy/mmd) not found, skipping cache seeding.")
        return False

    missing = [tok for tok, _d, _n in CACHE_SEED_FILES
               if not os.path.exists(_host_source(tok, cache_dir))]
    if missing:
        print(f"[CACHE] host files missing ({', '.join(missing)}); "
              "run tools/gen_font_atlas.py first; skipping seeding.")
        return False

    spec = f"{image}@@{part_offset}"
    # Create directories (failures mean they already exist).
    for dirname in ("::/FONTS", "::/CACHE", "::/EXAMPLES"):
        subprocess.run(["mmd", "-i", spec, dirname],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    for tok, dest_dir, dest_name in CACHE_SEED_FILES:
        src = _host_source(tok, cache_dir)
        if not os.path.exists(src):
            print(f"[CACHE] host file '{src}' missing, skipping.")
            ok = False
            continue
        r = subprocess.run(["mcopy", "-o", "-i", spec, src,
                            f"{dest_dir}/{dest_name}"])
        if r.returncode != 0:
            print(f"[CACHE] failed to seed '{src}' into '{image}:{dest_dir}/{dest_name}'")
            ok = False
        else:
            print(f"[CACHE] seeded '{image}:{dest_dir}/{dest_name}' ({os.path.getsize(src)} B)")
    return ok


def seed_caches(force=False, cache_dir="build/cache"):
    """Seed font + page-cache preload files into existing disk images."""
    seeded = False
    if os.path.exists("disk_mbr_fat32.img"):
        if seed_fat32_partition("disk_mbr_fat32.img", MBR_FAT32_PART_OFFSET, cache_dir):
            seeded = True
    if os.path.exists("disk_gpt_ext2.img"):
        # NOTE: the EXT2 root in a freshly created image is only a superblock
        # stub (see create_gpt_ext2_disk); it becomes a real filesystem only
        # after the in-kernel installer runs mkfs_ext2. Seed the ESP (FAT32),
        # and let the installer deploy /CACHE + /FONTS onto EXT2.
        if seed_fat32_partition("disk_gpt_ext2.img", ESP_PART_OFFSET, cache_dir):
            seeded = True
    if not seeded and not force:
        print("[CACHE] nothing seeded (images absent or tools/files missing).")
    return seeded

def create_mbr_fat32_disk(filename="disk_mbr_fat32.img", size_mb=64):
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return False

    print(f"[CREATE] Generating new {size_mb}MB FAT32 disk image: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)

    struct.pack_into('<H', disk, 510, 0xAA55)
    part_start_lba = 2048
    part_sectors = (img_size // 512) - part_start_lba
    
    struct.pack_into('<B', disk, 446 + 0, 0x80)
    struct.pack_into('<B', disk, 446 + 4, 0x0C)
    struct.pack_into('<I', disk, 446 + 8, part_start_lba)
    struct.pack_into('<I', disk, 446 + 12, part_sectors)

    fat_offset = part_start_lba * 512
    # FAT32-valid geometry: 1 sector/cluster on 64MB yields ~127k clusters
    # (>= 65525 required for FAT32; the old spc=8 layout was FAT16-sized).
    spc, reserved, num_fats = 1, 32, 2
    fat_size = ((part_sectors - reserved) * 4 + (512 * spc) - 1) // (512 * spc + (num_fats * 4))
    disk[fat_offset:fat_offset + 3] = b'\xEB\x58\x90'
    disk[fat_offset + 3:fat_offset + 11] = b'MSWIN4.1'
    struct.pack_into('<H', disk, fat_offset + 11, 512)
    disk[fat_offset + 13] = spc
    struct.pack_into('<H', disk, fat_offset + 14, reserved)
    disk[fat_offset + 16] = num_fats
    struct.pack_into('<H', disk, fat_offset + 24, 32)  # BPB_SecPerTrk
    struct.pack_into('<H', disk, fat_offset + 26, 64)  # BPB_NumHeads
    disk[fat_offset + 21] = 0xF8
    struct.pack_into('<I', disk, fat_offset + 28, part_start_lba)
    struct.pack_into('<I', disk, fat_offset + 32, part_sectors)
    struct.pack_into('<I', disk, fat_offset + 36, fat_size)
    struct.pack_into('<I', disk, fat_offset + 44, 2)
    struct.pack_into('<H', disk, fat_offset + 48, 1)
    struct.pack_into('<H', disk, fat_offset + 50, 6)
    disk[fat_offset + 66] = 0x29
    struct.pack_into('<I', disk, fat_offset + 67, 0x12345678)
    disk[fat_offset + 71:fat_offset + 82] = b'EQUANT FAT32'
    disk[fat_offset + 82:fat_offset + 90] = b'FAT32   '
    struct.pack_into('<H', disk, fat_offset + 510, 0xAA55)
    # Backup VBR (sector 6) + FSInfo (sectors 1 & 7)
    disk[fat_offset + (6 * 512):fat_offset + (7 * 512)] = disk[fat_offset:fat_offset + 512]
    total_clusters = (part_sectors - (reserved + num_fats * fat_size)) // spc
    for fsinfo_sec in (1, 7):
        fs_off = fat_offset + fsinfo_sec * 512
        struct.pack_into('<I', disk, fs_off, 0x41615252)
        struct.pack_into('<I', disk, fs_off + 484, 0x61417272)
        struct.pack_into('<I', disk, fs_off + 488, total_clusters - 1)
        struct.pack_into('<I', disk, fs_off + 492, 3)
        struct.pack_into('<I', disk, fs_off + 508, 0xAA550000)
    # FAT1 & FAT2 init (media + EOC + root cluster EOC)
    fat_init = b'\xF8\xFF\xFF\x0F\xFF\xFF\xFF\x0F\xFF\xFF\xFF\x0F'
    for f in range(num_fats):
        fat_off = fat_offset + (reserved + f * fat_size) * 512
        disk[fat_off:fat_off + 12] = fat_init
    # Zero root directory cluster (cluster 2 = first data sector)
    data_off = fat_offset + (reserved + num_fats * fat_size) * 512
    disk[data_off:data_off + spc * 512] = bytes(spc * 512)

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Created MBR + FAT32 Disk Image: '{filename}'")
    return True

def create_gpt_ext2_disk(filename="disk_gpt_ext2.img", size_mb=128):
    if os.path.exists(filename):
        print(f"[SKIP] Disk image '{filename}' already exists. Preserving data.")
        return False

    print(f"[CREATE] Generating spec-compliant {size_mb}MB GPT Dual-Boot disk: '{filename}'...")
    img_size = size_mb * 1024 * 1024
    disk = bytearray(img_size)
    total_sectors = img_size // 512

    # 1. Protective MBR (LBA 0)
    struct.pack_into('<H', disk, 510, 0xAA55)
    struct.pack_into('<B', disk, 446 + 4, 0xEE)
    struct.pack_into('<I', disk, 446 + 8, 1)
    struct.pack_into('<I', disk, 446 + 12, total_sectors - 1)

    # 2. Partition Entries Array (LBA 2..33, 128 entries * 128 bytes)
    entries_offset = 2 * 512
    entries_size = 128 * 128

    # Entry 0: ESP (FAT32, 40MB = 81920 sectors, >65525 clusters for FAT32 compliance!)
    esp_guid = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    esp_unique = uuid.UUID('11111111-1111-1111-1111-111111111111').bytes_le
    esp_start = 2048
    esp_sectors = 81920
    esp_end = esp_start + esp_sectors - 1
    esp_name = "EFI System Partition".encode('utf-16le')
    struct.pack_into('<16s16sQQQ72s', disk, entries_offset,
                     esp_guid, esp_unique, esp_start, esp_end, 0, esp_name)

    # Entry 1: Root (EXT2, 64MB = 131072 sectors)
    root_guid = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    root_unique = uuid.UUID('22222222-2222-2222-2222-222222222222').bytes_le
    root_start = esp_end + 1
    root_sectors = 131072
    root_end = root_start + root_sectors - 1
    root_name = "EquantOS Root".encode('utf-16le')
    struct.pack_into('<16s16sQQQ72s', disk, entries_offset + 128,
                     root_guid, root_unique, root_start, root_end, 0, root_name)

    # Calculate CRC32 of partition array
    part_array_crc = zlib.crc32(bytes(disk[entries_offset : entries_offset + entries_size])) & 0xFFFFFFFF
    disk_guid = uuid.UUID('33333333-3333-3333-3333-333333333333').bytes_le

    # 3. Primary GPT Header (LBA 1)
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, 512,
        b'EFI PART', 0x00010000, 92, 0, 0,
        1, total_sectors - 1, 34, total_sectors - 34,
        disk_guid, 2, 128, 128, part_array_crc
    )
    hdr_crc = zlib.crc32(bytes(disk[512 : 512 + 92])) & 0xFFFFFFFF
    struct.pack_into('<I', disk, 512 + 16, hdr_crc)

    # 4. Backup Partition Array (LBA total_sectors - 33)
    backup_entries_offset = (total_sectors - 33) * 512
    disk[backup_entries_offset : backup_entries_offset + entries_size] = disk[entries_offset : entries_offset + entries_size]

    # 5. Backup GPT Header (LBA total_sectors - 1)
    backup_hdr_offset = (total_sectors - 1) * 512
    struct.pack_into('<8sIIIIQQQQ16sQIII', disk, backup_hdr_offset,
        b'EFI PART', 0x00010000, 92, 0, 0,
        total_sectors - 1, 1, 34, total_sectors - 34,
        disk_guid, total_sectors - 33, 128, 128, part_array_crc
    )
    bhdr_crc = zlib.crc32(bytes(disk[backup_hdr_offset : backup_hdr_offset + 92])) & 0xFFFFFFFF
    struct.pack_into('<I', disk, backup_hdr_offset + 16, bhdr_crc)

    # 6. Format ESP (FAT32 BPB + FSInfo) at LBA 2048 (40MB, 80608 data clusters)
    esp_offset = esp_start * 512
    disk[esp_offset:esp_offset + 3] = b'\xEB\x58\x90'
    disk[esp_offset + 3:esp_offset + 11] = b'MSWIN4.1'
    struct.pack_into('<H', disk, esp_offset + 11, 512)
    disk[esp_offset + 13] = 1
    struct.pack_into('<H', disk, esp_offset + 14, 32)
    disk[esp_offset + 16] = 2
    struct.pack_into('<H', disk, esp_offset + 24, 32)  # BPB_SecPerTrk (mtools compat)
    struct.pack_into('<H', disk, esp_offset + 26, 64)  # BPB_NumHeads (mtools compat)
    disk[esp_offset + 21] = 0xF8
    struct.pack_into('<I', disk, esp_offset + 28, esp_start)
    struct.pack_into('<I', disk, esp_offset + 32, esp_sectors)
    struct.pack_into('<I', disk, esp_offset + 36, 640)
    struct.pack_into('<I', disk, esp_offset + 44, 2)
    struct.pack_into('<H', disk, esp_offset + 48, 1)
    struct.pack_into('<H', disk, esp_offset + 50, 6)
    disk[esp_offset + 66] = 0x29
    struct.pack_into('<I', disk, esp_offset + 67, 0x87654321)
    disk[esp_offset + 71:esp_offset + 82] = b'EFI SYSTEM '
    disk[esp_offset + 82:esp_offset + 90] = b'FAT32   '
    struct.pack_into('<H', disk, esp_offset + 510, 0xAA55)
    disk[esp_offset + (6 * 512) : esp_offset + (7 * 512)] = disk[esp_offset : esp_offset + 512]

    # FSInfo Sector 1 & 7
    struct.pack_into('<I', disk, esp_offset + 512, 0x41615252)
    struct.pack_into('<I', disk, esp_offset + 512 + 484, 0x61417272)
    struct.pack_into('<I', disk, esp_offset + 512 + 488, 80000)
    struct.pack_into('<I', disk, esp_offset + 512 + 492, 3)
    struct.pack_into('<I', disk, esp_offset + 512 + 508, 0xAA550000)
    disk[esp_offset + (7 * 512) : esp_offset + (8 * 512)] = disk[esp_offset + 512 : esp_offset + 1024]

    # FAT1 & FAT2 Root Clusters
    fat1_off = esp_offset + (32 * 512)
    fat2_off = fat1_off + (640 * 512)
    fat_init = b'\xF8\xFF\xFF\x0F\xFF\xFF\xFF\x0F\xFF\xFF\xFF\x0F'
    disk[fat1_off : fat1_off + 12] = fat_init
    disk[fat2_off : fat2_off + 12] = fat_init

    # 7. Format Root (EXT2 Superblock) at LBA 83968
    root_offset = root_start * 512
    sb_offset = root_offset + 1024
    total_blocks = (root_sectors * 512) // 1024
    struct.pack_into('<I', disk, sb_offset + 0, 1024)
    struct.pack_into('<I', disk, sb_offset + 4, total_blocks)
    struct.pack_into('<I', disk, sb_offset + 12, total_blocks - 134)
    struct.pack_into('<I', disk, sb_offset + 16, 1024 - 11)
    struct.pack_into('<I', disk, sb_offset + 20, 1)
    struct.pack_into('<I', disk, sb_offset + 24, 0)
    struct.pack_into('<I', disk, sb_offset + 32, 8192)
    struct.pack_into('<I', disk, sb_offset + 40, 1024)
    struct.pack_into('<H', disk, sb_offset + 56, 0xEF53)
    struct.pack_into('<H', disk, sb_offset + 58, 1)
    struct.pack_into('<I', disk, sb_offset + 84, 11)
    struct.pack_into('<H', disk, sb_offset + 88, 128)
    disk[sb_offset + 120:sb_offset + 131] = b'EQUANT_EXT2'

    with open(filename, "wb") as f:
        f.write(disk)
    print(f"[SUCCESS] Spec-compliant GPT disk created: ESP (40M) + EXT2 (64M) + Free Space (24M)!")
    return True

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Create EquantOS disk images + seed caches")
    ap.add_argument("--force", action="store_true",
                    help="recreate images even if they exist (destroys data)")
    ap.add_argument("--reseed", action="store_true",
                    help="seed cache files into existing images without recreating")
    ap.add_argument("--no-seed", action="store_true", help="do not seed cache files")
    ap.add_argument("--cache-dir", default="build/cache", help="host cache files dir")
    ap.add_argument("--gen-cache", action="store_true", default=True,
                    help="run tools/gen_font_atlas.py first (default on)")
    ap.add_argument("--no-gen-cache", action="store_false", dest="gen_cache",
                    help="skip running tools/gen_font_atlas.py")
    args = ap.parse_args()

    if args.force:
        for victim in ("disk_mbr_fat32.img", "disk_gpt_ext2.img"):
            if os.path.exists(victim):
                os.remove(victim)
                print(f"[CREATE] Removed existing '{victim}' (--force).")

    if args.gen_cache and os.path.exists(os.path.join(REPO_ROOT, "tools/gen_font_atlas.py")) \
            and os.path.exists(os.path.join(REPO_ROOT, "res/font.psf")):
        os.makedirs(args.cache_dir, exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(REPO_ROOT, "tools/gen_font_atlas.py"),
                            "--font", os.path.join(REPO_ROOT, "res/font.psf"),
                            "--out-dir", args.cache_dir])
        if r.returncode != 0:
            print("[CACHE] font atlas generation failed; continuing without seeding.")

    created_mbr = create_mbr_fat32_disk()
    created_gpt = create_gpt_ext2_disk()

    if not args.no_seed and (created_mbr or created_gpt or args.reseed):
        seed_caches(force=args.reseed, cache_dir=args.cache_dir)
