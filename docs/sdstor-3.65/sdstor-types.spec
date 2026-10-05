# SceSdstor (os0:kd/sdstor.skprx, 3.65) structures
struct sds_dev 0x54
  0x40 u8 kind          # 0 internal eMMC, 1 game-card slot, 0xFF no SD bus (memory card / USB), 3 device 4 SD
  0x41 u8 bus           # 0 MMC, 1 SD, 2 memory card (Msif), 3 USB mass
  0x42 u8 state         # 0 down, 1 ready, 2 removed
  0x43 u8 wprot         # write-protected
  0x44 u32 sector_size
  0x48 u32 nsectors
  0x4c u32 info
  0x50 ptr ctx          # Sdif controller context
end
struct sds_part 0x1c
  0x0 u32 start
  0x4 u32 nsectors
  0x8 ptr owner         # sds_slot (partition table of a device)
  0xc ptr raw           # Sony MBR entry (17 bytes) or MBR entry
  0x10 char[4] name     # "NNN"
  0x14 u8[6] rights     # SceSblACMgr type-8 rights
end
struct sds_handle 0x58
  0x40 u32 sector_size
  0x44 u32 start
  0x48 u32 nsectors
  0x4c u32 flags        # 1 read, 2 write
  0x50 sds_part* part
  0x54 sds_dev* dev
end
global 0x81008558 sds_dev[5] sds_devices
