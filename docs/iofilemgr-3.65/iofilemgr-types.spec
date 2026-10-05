# SceIofilemgr (3.65) core structures; field names follow psp2kern/vfs.h
struct SceVfsMount 0x100
  0x40 SceVfsVnode* mnt_vnode
  0x44 s32 allocator
  0x48 u32 state           # 0x10 = unmounting / invalidated
  0x4c u8 fs_type
  0x4e u16 opt
  0x50 u32 mnt_flags
  0x54 SceVfsVnode* vnode_list
  0x58 u32 vnode_num
  0x5c ptr mnt_vfs_inf
  0x60 u32 mnt_ref_count
  0x64 u32 opened_entry_num
  0x68 u32 available_entry_num
  0x6c s32 pid
  0x70 SceVfsMount* mnted_on_list
  0x74 SceVfsMount* mnted_on_list_prev
  0x78 SceVfsMount* mnt_list_next
  0x7c ptr mnt_data
  0x80 char[64] path
  0xc0 u32 default_io_cache_size
  0xc4 ptr data
  0xc8 ptr fd_lock
  0xcc ptr fumount
end
struct SceVfsVnode 0x100
  0x40 ptr ops
  0x44 u32 node_inf
  0x48 ptr node_data
  0x4c SceVfsMount* mnt
  0x50 SceVfsVnode* dd
  0x54 SceVfsVnode* next
  0x58 u32 ref_count
  0x5c ptr bc
  0x60 u32[2] fid
  0x68 ptr flock
  0x6c s32 allocator
  0x70 ptr ncache
  0x74 u32 state
  0x78 u32 type
  0x7c ptr vop_tbl
  0x80 u64 size
  0x88 u32[2] acl_data
  0x90 SceVfsFile* fd_list
  0x94 u32 fd_num
  0x98 SceVfsVnode* link_to
  0x9c u32 linked_num
  0xd0 u32 mnt_opt
  0xd4 u32 unk_d4
end
struct SceVfsFile 0x40
  0x0 u32 is_dir
  0x4 u32 flags
  0x8 u64 position
  0x10 u32 state
  0x14 s32 pid
  0x18 SceVfsVnode* vp
  0x1c SceVfsFile* next
  0x20 u32 fd
  0x24 u16 flock_busy_count
  0x26 u8 is_locked
  0x27 u8 has_flock_ent
  0x28 ptr fd_lock
  0x2c u32 idata
  0x30 ptr debug_path
end
# SceVfsMountParam as passed to vfsMount (also the body of an assign entry)
struct SceVfsMountParam 0x20
  0x0 ptr root_path        # e.g. "/uma/exfat"
  0x4 ptr blockdev_name    # NULL: misc->blockdev, then misc->blockdev_no_part
  0x8 u8 fs_type
  0xa u16 opt              # I/O class / scheduler queue id (0x100, 0x201..0x203, 0x300)
  0xc u32 mnt_flags        # low byte = mount type (1 PFS, 2 FSROOT, 3 DEVFS, 5 STACKFS, 6 HOSTFS)
  0x10 ptr vfs_name        # "exfat", "sdstor_dev_fs", ...
  0x14 ptr data
  0x18 ptr misc            # assign info {assign, unit name, blockdev, blockdev_no_part}
  0x1c ptr vops
end
# assign table entry (32 at 0x81022b30)
struct iof_assign 0x38
  0x0 s32 mount_id
  0x4 SceVfsMountParam param
  0x24 ptr info             # first word: assign name ("uma0:")
  0x28 ptr mount_events     # iof_mntev list (+0x48 next)
  0x2c iof_assign* children # assigns mounted on this one
  0x30 iof_assign* next_sibling
  0x34 iof_assign* parent
end
global 0x81022b30 iof_assign[32] iof_assign_table
# mount request handled by the SceIofilemgrMount thread
struct iof_mntreq 0x24
  0x0 s32 type              # 1 mount id, 2 umount id, 3 mount param (PFS), 4 umount path (PFS)
  0x4 s32 mount_id
  0x8 ptr path              # blockdev / param / {path, flags} / umount flags
  0xc u32 flags             # mount: 1 RDONLY, 2 clear RDONLY, others ORed into mnt_flags
  0x10 ptr data
  0x14 s32 pid
  0x1c s32 tid              # woken with ksceKernelSignalCondTo
  0x20 s32 result
end
# I/O scheduler request (embedded at +0x38 of a 0xf8-byte SceIoAsyncEvent)
struct iof_req 0xb8
  0x0 ptr next              # priority list links
  0x4 ptr prev
  0x8 ptr chunks_head
  0xc ptr chunks_tail
  0x10 u16 chunks_made
  0x12 u16 chunks_running
  0x14 u16 chunks_total
  0x16 u8 waiter
  0x17 u8 parked
  0x18 ptr xfer_buf
  0x1c u32 xfer_done
  0x20 u32 xfer_left
  0x24 s32 op               # 1 open .. 0x1d, see iof_req_* builders
  0x28 s32 prio
  0x2c u32 queue            # mount opt; bit 16 = PFS facade
  0x30 u8 async
  0x31 u8 done
  0x32 u8 running
  0x33 u8 stop
  0x34 s32 caller_pid
  0x38 s32 owner_tid
  0x3c s32 tls_pid
  0x40 u32 permission
  0x44 u32 affinity
  0x48 s32 thread_prio
  0x4c ptr chunk_pool
  0x5c u16 from_user
  0x5e u16 bulk
  0x60 s32 event_uid
  0x64 u32 arg0             # path or fd
  0x68 u32 arg1
  0x6c u32 arg2
  0x70 u32 arg3             # transfer length
  0x74 u32 arg4             # user mapping uid for transfers
  0x78 u64 offset           # pread/pwrite offset, write result
  0x80 s32 xfer_result
  0x88 ptr async_param      # SceIoAsyncParam of the caller
  0x8c u32 async_param_word
  0x90 u64 result
  0x98 u32 removable_meta
  0x9c u32 bulk_chunk
  0xa0 u32 first_misalign
  0xa4 iof_req* run_next
  0xa8 u64 cached_pos
  0xb0 u64 submit_time
end
struct iof_chunk 0x14
  0x0 iof_chunk* next
  0x4 ptr buf
  0x8 u32 off
  0xc u32 len
  0x10 iof_req* req
end
struct iof_plist 0x14
  0x0 ptr queue
  0x4 s32 prio
  0x8 iof_req* head
  0xc iof_req* tail
  0x10 u32 age_us
end
struct iof_queue 0xa8
  0x0 u32 id
  0x4 iof_plist*[16] lists
  0x44 u32 read_chunk
  0x48 u32 write_chunk
  0x4c ptr sched
  0x50 s32 mutex
  0x54 s32 cond
  0x58 s8 running
  0x5a s8 kicks
  0x60 s32 worker_tid
  0x6c iof_queue* self
  0x70 s32 pending
  0x74 u16 stop
  0x76 u16 owner_waiting
  0x78 u32 throttle_us
  0x7c iof_req* last
  0x80 s32 helpers
  0x84 u32 event_bits
  0x88 u64 bulk_seen
  0x90 u64 throttle_start
  0xa0 iof_req* run_head
  0xa4 iof_req* run_tail
end
