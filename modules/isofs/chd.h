#ifndef OPL_EXPERIMENT_CHD_H
#define OPL_EXPERIMENT_CHD_H
#include <stdint.h>
#ifdef CHD_HOST_TEST
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <tamtypes.h>
#endif
/* Experimental CHD v5 DVD reader: zlib-only, raw hunks and self references. */
#define OPL_CHD_MAGIC "MComprHD"
#define OPL_CHD_CODEC_ZLIB 0x7a6c6962U
int chd_init(void);
void chd_close(void);
int chd_active(void);
u64 chd_logical_bytes(void);
u32 chd_sector_count(void);
int chd_read_sector(u8 *buffer, u32 lsn, unsigned int count);
void *chd_alloc(u32 size);
void chd_free(void *ptr);
int chd_read_raw_data(u8 *addr, u32 size, u32 offset);
#endif
