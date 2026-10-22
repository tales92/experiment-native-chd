#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../modules/isofs/chd.h"
static FILE *chd_file;
void *chd_alloc(u32 n){return malloc(n);}
void chd_free(void *p){free(p);}
int chd_read_raw_data(u8 *p,u32 n,u32 off){if(fseek(chd_file,(long)off,SEEK_SET))return -1;return (int)fread(p,1,n,chd_file);}
int main(int argc,char **argv){
    FILE *iso; u8 *got,*want; u32 sectors,lsn,batch=17; unsigned long long bytes; int fail=0;
    if(argc!=3){fprintf(stderr,"usage: %s image.chd source.iso\n",argv[0]);return 2;}
    chd_file=fopen(argv[1],"rb"); if(!chd_file){perror("open CHD");return 2;}
    if(!chd_init()){fprintf(stderr,"unsupported/invalid CHD v5: expected zlib DVD CHD with 4K-256K hunks\n");fclose(chd_file);return 1;}
    iso=fopen(argv[2],"rb"); if(!iso){perror("open ISO");chd_close();fclose(chd_file);return 2;}
    fseek(iso,0,SEEK_END); bytes=(unsigned long long)ftell(iso); rewind(iso); sectors=chd_sector_count();
    if(bytes!=(unsigned long long)sectors*2048ULL){fprintf(stderr,"logical size mismatch\n");fail=1;}
    got=(u8*)malloc(batch*2048U); want=(u8*)malloc(batch*2048U);
    if(!got||!want){fprintf(stderr,"test out of memory\n");fail=1;}
    for(lsn=0;!fail&&lsn<sectors;){
        u32 n=sectors-lsn<batch?sectors-lsn:batch;
        if(chd_read_sector(got,lsn,n)!=(int)n){fprintf(stderr,"decode failed at LSN %u\n",lsn);fail=1;break;}
        if(fseek(iso,(long)((unsigned long long)lsn*2048ULL),SEEK_SET)||fread(want,2048U,n,iso)!=n){fprintf(stderr,"ISO read failed at LSN %u\n",lsn);fail=1;break;}
        if(memcmp(got,want,n*2048U)){fprintf(stderr,"data mismatch at LSN %u\n",lsn);fail=1;break;}
        lsn+=n;
    }
    if(!fail)printf("PASS: %u sectors compared byte-for-byte (%llu bytes)\n",sectors,bytes);
    free(got);free(want);fclose(iso);chd_close();fclose(chd_file);return fail?1:0;
}
