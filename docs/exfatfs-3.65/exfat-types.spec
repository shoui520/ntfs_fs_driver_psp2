# exfatfs 3.65 type spec, consumed by scripts/ApplyTypes.java
struct exfat_mnt 0x230
  0x0 u32 flags            # 1 mounted, 0x20 mount error, 0x1000 32-bit FAT (FAT32/exFAT), 0x2000 set while mounting, 0x4000 invalidated (forced umount), 0x8000 FAT16
  0xc0 char* assign_name   # copy of mnt_data->assign_name
  0xd0 ptr ops             # never written in this module; only used by dead code (mnt_reactivate)
  0xd4 ptr[64] slots       # never written; dead code clears slot->+0x684
  0x1e0 u8[0x40] lock      # SceKernelFastMutex "SceExfatfsDrive<X>"
  0x220 ptr mnt_data       # SceVfsMountData*
  0x224 ptr blk_vp         # block device vnode
  0x228 ptr blk_file       # SceVfsFile* opened on it
  0x22c s32 blk_fd         # its UID
end
struct uvfat_fnode 0x290
  0x0 u16[0x104] path
  0x208 u16* name
  0x20c uvfat_drive* drive
  0x210 u16 mode
  0x212 u16 type           # 0 regular file, 6 directory
  0x214 u16 unk_214
  0x216 u16 unk_216
  0x218 u64 size          # DataLength
  0x220 u8[12] time_c
  0x22c u8[12] time_a
  0x238 u8[12] time_m
  0x244 u32 first_cluster
  0x248 u8 sec_flags       # exFAT GeneralSecondaryFlags (2 = NoFatChain)
  0x249 u8 attr
  0x250 u32 parent_cluster   # 0xFFFFFFFE: parent directory no longer exists
  0x258 u64 parent_pos
  0x260 u32 parent_cluster2
  0x268 u32 parent_size
  0x26c u8 parent_flags
  0x26d u8 dirty           # 4 = mtime
  0x270 u16 ref
  0x272 u16 nopen          # open fds; last close of an orphan frees clusters
  0x278 u32 ccache_n
  0x27c ptr ccache
  0x280 u32 cur_clus
  0x284 u32 cur_idx
  0x288 u64 cur_pos
end
struct uvfat_drive 0x430
  0x0 char* dev            # block device name; NULL = free slot
  0x4 u32 mode
  0x8 u32 state            # 2 = volume mounted
  0xc u32 data_start       # first sector of cluster heap / data region
  0x10 u16 rootdir_secs    # FAT12/16 fixed root directory sectors
  0x14 u32 nclusters
  0x18 u8* secbuf          # 0x200 sector buffer (g->secbufs); holds sector 0 (boot sector)
  0x1c u32 boot_dirty
  0x20 u32 fat_free
  0x24 u32 fat_free_valid
  0x28 ptr fatwin_lru
  0x2c u32[28] fatwin     # 4 x {first, count, buf, raw, start_sector, dirty, next}
  0x9c u32 fatcache_sz
  0xa0 u32 fat_dirty
  0xa4 u32 fatbits         # 12/16/32
  0xa8 u32 fat_eoc         # cluster value >= this is EOC/bad (0xFFFFFFF7 exFAT)
  0xac exfat_mnt* mnt
  0xb0 u32 opts            # low byte 1 ro / 2 rw, 0x100, 0x200 cache whole FAT, 0x400 cluster cache
  0xb4 u32 fstype          # 1 FAT12/16/32, 2 exFAT
  0xb8 u32 bps_shift
  0xbc u32 bytes_per_sec
  0xc0 u32 sec_per_clus
  0xc4 u32 root_cluster
  0xc8 u64 vol_sectors
  0xd0 u8 bitmap_index
  0xd1 u8 bitmap_dirty
  0xd4 u32 bitmap_cluster
  0xd8 u64 bitmap_size
  0xe0 u8* bitmap_buf
  0xe4 u8* bitmap_buf_raw
  0xe8 u64 bitmap_lba
  0xf0 u32 bitmap_clus
  0xf4 u32 free_clusters
  0xf8 u16* upcase
  0x100 u64 upcase_size
  0x108 ptr upcase_runs
  0x10c ptr upcase_runs_tail
  0x110 ptr upcase_chunks
  0x114 uvfat_fnode* trash      # pinned SceIoTrash directory
  0x118 uvfat_fd dirit           # embedded fd: drive-private directory iterator on the root fnode (mode 3 while in use)
  0x150 uvfat_fnode root
  0x3e0 u8[0x40] lock
  0x420 u32 io_size        # min(cluster bytes, 0x200)
  0x424 u32 free_hint
  0x428 u32 reserved_clusters  # devctl 0x3004 quota
end
struct uvfat_fd 0x38
  0x0 uvfat_fnode* fnode
  0x4 u32 mode            # 0 free, 1 read, 2 write, 6 read/write, 3 directory
  0x8 u64 pos
  0x10 u8* buf
  0x18 u32 clus
  0x1c u32 clus_idx
  0x14 u8* buf_raw
  0x20 u32 flags           # 1 = buffer dirty
  0x28 u64 buf_block
  0x30 u32 buf_clus
  0x34 u32 unk_34
end
struct uvfat_global 0x295d60
  0x0 uvfat_drive[15] drive
  0x3ed0 u8[0x1e00] secbufs  # 15 x 0x200
  0x5cd0 uvfat_fd* fds
  0x5cd8 uvfat_fnode[0x400] fnode
  0x295cd8 u32[30] pins   # 15 x {active, drive*}
  0x295d50 u32 fnode_cap   # 0x1000
  0x295d54 u32 nfnodes
  0x295d58 u32 fd_cap
  0x295d5c u32 nfds
end
global 0x8101d080 uvfat_global g
