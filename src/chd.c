#include <stdlib.h>
#include <tamtypes.h>
#include "../modules/isofs/chd.h"
extern int read_raw_data(u8 *addr,u32 size,u32 offset,u32 shift);
void *chd_alloc(u32 n){return malloc(n);}
void chd_free(void *p){free(p);}
int chd_read_raw_data(u8 *p,u32 n,u32 off){return read_raw_data(p,n,off,0);}
#include "../modules/isofs/chd.c"
