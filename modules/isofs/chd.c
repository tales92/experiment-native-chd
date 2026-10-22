/*
 * Experimental CHD v5 reader for OPL.
 * Scope: DVD-style CHD v5, 2048-byte units, zlib codec slot 0, raw hunks and
 * self-hunk aliases. Parent CHDs and other codecs are rejected.
 * Map decoding is derived from libchdr (BSD-3-Clause); tinfl comes from miniz.
 */
#include <stdint.h>
#include <string.h>
#include "chd.h"
#include "chd_bitstream.h"
#include "chd_huffman.h"
#define MINIZ_NO_MALLOC
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "chd/miniz.h"
#define CHD_HEADER_SIZE 124U
#define CHD_ENTRY_SIZE 12U
#define CHD_MAP_HEADER_SIZE 16U
#define CHD_MAX_MAP_BYTES (2U * 1024U * 1024U)
#define CHD_MAX_HUNKS 40000U
#define CHD_MIN_HUNK_BYTES 4096U
#define CHD_MAX_HUNK_BYTES 262144U
#define CHD_SECTOR_SIZE 2048U
#define TYPE_0 0U
#define TYPE_1 1U
#define TYPE_2 2U
#define TYPE_3 3U
#define TYPE_NONE 4U
#define TYPE_SELF 5U
#define RLE_SMALL 7U
#define RLE_LARGE 8U
#define SELF_0 9U
#define SELF_1 10U
static u8 *g_map, *g_hunk_buf, *g_comp_buf;
static u64 g_logical_bytes;
static u32 g_hunk_bytes, g_hunk_count, g_sector_count;
static u32 g_cached_hunk = 0xffffffffU;
static int g_active;
static tinfl_decompressor g_inflater;
static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 be24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 be48(const u8 *p) { return ((u64)p[0] << 40) | ((u64)p[1] << 32) | ((u64)p[2] << 24) | ((u64)p[3] << 16) | ((u64)p[4] << 8) | p[5]; }
static u64 be64(const u8 *p) { return ((u64)be32(p) << 32) | be32(p + 4); }
static void put24(u8 *p, u32 v) { p[0]=(u8)(v>>16); p[1]=(u8)(v>>8); p[2]=(u8)v; }
static void put48(u8 *p, u64 v) { p[0]=(u8)(v>>40); p[1]=(u8)(v>>32); p[2]=(u8)(v>>24); p[3]=(u8)(v>>16); p[4]=(u8)(v>>8); p[5]=(u8)v; }
static u16 crc16_ccitt(const u8 *p, u32 n)
{
    u32 i,j; u16 crc=0xffffU;
    for(i=0;i<n;i++){ crc^=(u16)((u16)p[i]<<8); for(j=0;j<8;j++) crc=(u16)((crc&0x8000U)?((crc<<1)^0x1021U):(crc<<1)); }
    return crc;
}
static int decode_map(u64 map_offset)
{
    u8 mh[CHD_MAP_HEADER_SIZE], *packed=NULL;
    struct bitstream *bits=NULL;
    struct huffman_decoder *decoder=NULL;
    u32 map_bytes, i, rep_count=0, last_self=0;
    u64 current_offset, first_offset;
    u16 map_crc;
    u8 length_bits,self_bits,parent_bits;
    int ok=0;
    if(map_offset>UINT32_MAX-CHD_MAP_HEADER_SIZE) return 0;
    if(chd_read_raw_data(mh,sizeof(mh),(u32)map_offset)!=(int)sizeof(mh)) return 0;
    map_bytes=be32(mh); first_offset=be48(mh+4); map_crc=be16(mh+10);
    length_bits=mh[12]; self_bits=mh[13]; parent_bits=mh[14];
    if(!map_bytes || map_bytes>CHD_MAX_MAP_BYTES ||
       map_offset+CHD_MAP_HEADER_SIZE+map_bytes>UINT32_MAX ||
       length_bits>24 || self_bits>32 || parent_bits>32) return 0;
    packed=(u8*)chd_alloc(map_bytes);
    g_map=(u8*)chd_alloc(g_hunk_count*CHD_ENTRY_SIZE);
    if(!packed || !g_map) goto done;
    if(chd_read_raw_data(packed,map_bytes,(u32)(map_offset+CHD_MAP_HEADER_SIZE))!=(int)map_bytes) goto done;
    memset(g_map,0,g_hunk_count*CHD_ENTRY_SIZE);
    bits=create_bitstream(packed,map_bytes); decoder=create_huffman_decoder(16,8);
    if(!bits || !decoder || huffman_import_tree_rle(decoder,bits)!=HUFFERR_NONE) goto done;
    for(i=0;i<g_hunk_count;i++){
        u8 type;
        if(rep_count){ type=g_map[(i-1)*CHD_ENTRY_SIZE]; rep_count--; }
        else {
            type=(u8)huffman_decode_one(decoder,bits);
            if(type==RLE_SMALL){
                if(!i) goto done;
                type=g_map[(i-1)*CHD_ENTRY_SIZE];
                rep_count=2U+huffman_decode_one(decoder,bits);
            }else if(type==RLE_LARGE){
                if(!i) goto done;
                type=g_map[(i-1)*CHD_ENTRY_SIZE];
                rep_count=18U+(huffman_decode_one(decoder,bits)<<4);
                rep_count+=huffman_decode_one(decoder,bits);
            }
            if(rep_count>g_hunk_count-i-1) goto done;
        }
        if(type>13U) goto done;
        g_map[i*CHD_ENTRY_SIZE]=type;
        if(bitstream_overflow(bits)) goto done;
    }
    current_offset=first_offset;
    for(i=0;i<g_hunk_count;i++){
        u8 *e=g_map+i*CHD_ENTRY_SIZE;
        u32 type=e[0], len=0, crc=0;
        u64 off=current_offset;
        if(type<=TYPE_3){
            if(type!=TYPE_0) goto done;
            len=bitstream_read(bits,length_bits); crc=bitstream_read(bits,16);
            current_offset+=len;
        }else if(type==TYPE_NONE){
            len=g_hunk_bytes; crc=bitstream_read(bits,16); current_offset+=len;
        }else if(type==TYPE_SELF){
            last_self=bitstream_read(bits,self_bits); off=last_self;
        }else if(type==SELF_0){
            e[0]=TYPE_SELF; off=last_self;
        }else if(type==SELF_1){
            last_self++; e[0]=TYPE_SELF; off=last_self;
        }else goto done; /* parent refs and non-zlib codec slots */
        if(len>0xffffffU || off>0xffffffffffffULL ||
           ((type<=TYPE_3) && (!len || current_offset>UINT32_MAX)) ||
           ((type==TYPE_NONE) && current_offset>UINT32_MAX)) goto done;
        put24(e+1,len); put48(e+4,off); e[10]=(u8)(crc>>8); e[11]=(u8)crc;
        if(bitstream_overflow(bits)) goto done;
    }
    if(crc16_ccitt(g_map,g_hunk_count*CHD_ENTRY_SIZE)!=map_crc) goto done;
    ok=1;
done:
    if(decoder) delete_huffman_decoder(decoder);
    if(bits) chd_free(bits);
    if(packed) chd_free(packed);
    if(!ok && g_map){ chd_free(g_map); g_map=NULL; }
    return ok;
}
void chd_close(void)
{
    if(g_map) chd_free(g_map);
    if(g_hunk_buf) chd_free(g_hunk_buf);
    if(g_comp_buf) chd_free(g_comp_buf);
    g_map=NULL; g_hunk_buf=NULL; g_comp_buf=NULL;
    g_logical_bytes=0; g_hunk_bytes=0; g_hunk_count=0; g_sector_count=0;
    g_cached_hunk=0xffffffffU; g_active=0;
}
int chd_init(void)
{
    u8 h[CHD_HEADER_SIZE];
    u32 version, header_bytes, c0,c1,c2,c3,unit_bytes,i;
    u32 sectors_per_hunk;
    u64 map_offset,sectors64;
    chd_close();
    if(chd_read_raw_data(h,sizeof(h),0)!=(int)sizeof(h) || memcmp(h,OPL_CHD_MAGIC,8)!=0) return 0;
    header_bytes=be32(h+8); version=be32(h+12);
    c0=be32(h+16); c1=be32(h+20); c2=be32(h+24); c3=be32(h+28);
    g_logical_bytes=be64(h+32); map_offset=be64(h+40);
    g_hunk_bytes=be32(h+56); unit_bytes=be32(h+60);
    if(header_bytes!=CHD_HEADER_SIZE || version!=5 || c0!=OPL_CHD_CODEC_ZLIB ||
       c1 || c2 || c3 || unit_bytes!=CHD_SECTOR_SIZE || !g_logical_bytes ||
       (g_logical_bytes & (CHD_SECTOR_SIZE - 1U)) != 0 ||
       g_hunk_bytes<CHD_MIN_HUNK_BYTES ||
       g_hunk_bytes>CHD_MAX_HUNK_BYTES || g_hunk_bytes%CHD_SECTOR_SIZE ||
       map_offset>UINT32_MAX) goto fail;
    for(i=104;i<124;i++) if(h[i]!=0) goto fail; /* no parent CHD */
    sectors64 = g_logical_bytes >> 11;
    if (!sectors64 || sectors64 > UINT32_MAX) goto fail;
    g_sector_count = (u32)sectors64;
    sectors_per_hunk = g_hunk_bytes >> 11;
    if (!sectors_per_hunk) goto fail;
    g_hunk_count = g_sector_count / sectors_per_hunk;
    if (g_sector_count % sectors_per_hunk)
        g_hunk_count++;
    if (!g_hunk_count || g_hunk_count > CHD_MAX_HUNKS) goto fail;
    if(!decode_map(map_offset)) goto fail;
    g_hunk_buf=(u8*)chd_alloc(g_hunk_bytes);
    g_comp_buf=(u8*)chd_alloc(g_hunk_bytes);
    if(!g_hunk_buf || !g_comp_buf) goto fail;
    g_active=1; return 1;
fail:
    chd_close(); return 0;
}
int chd_active(void){return g_active;}
u64 chd_logical_bytes(void){return g_logical_bytes;}
u32 chd_sector_count(void){return g_sector_count;}
static int load_hunk(u32 requested)
{
    u32 hunk=requested, attempts;
    if(!g_active || requested>=g_hunk_count) return 0;
    if(g_cached_hunk==requested) return 1;
    for(attempts=0;attempts<g_hunk_count;attempts++){
        u8 *e=g_map+hunk*CHD_ENTRY_SIZE;
        u32 type=e[0]; u64 off=be48(e+4);
        if(type==TYPE_SELF){
            if(off>=g_hunk_count || off==hunk) return 0;
            hunk=(u32)off; continue;
        }
        if(type==TYPE_NONE){
            if(off>UINT32_MAX-g_hunk_bytes ||
               chd_read_raw_data(g_hunk_buf,g_hunk_bytes,(u32)off)!=(int)g_hunk_bytes) return 0;
        }else if(type==TYPE_0){
            u32 compressed=be24(e+1); size_t in_size,out_size; tinfl_status status;
            if(!compressed || compressed>g_hunk_bytes || off>UINT32_MAX-compressed ||
               chd_read_raw_data(g_comp_buf,compressed,(u32)off)!=(int)compressed) return 0;
            tinfl_init(&g_inflater); in_size=compressed; out_size=g_hunk_bytes;
            status=tinfl_decompress(&g_inflater,(const mz_uint8*)g_comp_buf,&in_size,
                (mz_uint8*)g_hunk_buf,(mz_uint8*)g_hunk_buf,&out_size,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if(status!=TINFL_STATUS_DONE || out_size!=g_hunk_bytes) return 0;
        }else return 0;
        g_cached_hunk=requested; return 1;
    }
    return 0;
}
int chd_read_sector(u8 *buffer,u32 lsn,unsigned int count)
{
    u32 done=0;
    if(!g_active || !buffer || !count || lsn>=g_sector_count) return 0;
    if (count > g_sector_count - lsn)
        count = g_sector_count - lsn;
    while (done < count) {
        u32 sector_pos = lsn + done;
        u32 sectors_per_hunk = g_hunk_bytes >> 11;
        u32 hunk = sector_pos / sectors_per_hunk;
        u32 sector_in_hunk = sector_pos % sectors_per_hunk;
        u32 in_hunk = sector_in_hunk * CHD_SECTOR_SIZE;
        u32 available_sectors = sectors_per_hunk - sector_in_hunk;
        u32 remaining_sectors = count - done;
        u32 copy_sectors = available_sectors < remaining_sectors ? available_sectors : remaining_sectors;
        u32 bytes = copy_sectors * CHD_SECTOR_SIZE;
        if (!load_hunk(hunk))
            return (int)done;
        memcpy(buffer + done * CHD_SECTOR_SIZE, g_hunk_buf + in_hunk, bytes);
        done += copy_sectors;
    }
    return (int)done;
}

/* Compile the small decoder dependencies into this object on EE and IOP. */
#include "chd_bitstream.c"
#include "chd_huffman.c"
#include "chd/miniz.c"
