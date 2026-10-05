# SceSblACMgr (os0:kd/acmgr.skprx, 3.65) structures
struct acm_ctx 0xc
  0x0 u32 type          # 1..5 FAT-style attribute (exfatfs passes 2 or 3), 6 PFS-style, 7 unsupported, 8 six-byte rights
  0x4 ptr data          # the attribute, 1, 2 or 6 bytes
  0x8 u32 count         # size of data
end
struct SceSelfAuthInfo 0x90
  0x0 u64 program_authority_id
  0x8 u8[8] padding
  0x10 u8[0x20] capability   # bit n = capability[n>>3] >> (7-(n&7)); word 0 doubles as program class 0x80/0x40/0x20/0x10
  0x30 u8[0x20] attribute    # same numbering; bit 0x81 restricts vs0: and removable paths, 0x10 marks non-debuggable
  0x50 u8[0x40] secret
end
