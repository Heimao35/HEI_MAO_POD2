/* Production animation store: bounded allocations, exact NOR write semantics,
 * simulated boot/reset, interrupted slot updates and cache-only frame iteration.
 * Synthetic JPEGs exercise structure, not hardware entropy decoding. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p,0700)
#endif
#define SD_MOUNT_POINT "./animation-fixture"
#include "../../main/lock_animation.c"

static uint8_t flash[2*SLOT_BYTES];
static esp_partition_t partition={.size=sizeof(flash),.erase_size=SECTOR_BYTES};
static bool partition_present=true,mounted=true,present=true,sd_locked,runtime;
static unsigned reads,writes,erases,sd_locks;
static int fail_write,fail_erase,fail_read;
static bool corrupt_payload,fail_alloc;
static size_t live,peak;

typedef union { size_t size; long double align; } allocation_t;
void *heap_caps_malloc(size_t bytes,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(bytes<=LOCK_ANIMATION_MAX_BYTES);
    if(fail_alloc){fail_alloc=false;return NULL;}
    allocation_t *a=malloc(sizeof(*a)+bytes);assert(a);
    a->size=bytes;live+=bytes;if(live>peak)peak=live;
    return a+1;
}
void heap_caps_free(void *p)
{if(p){allocation_t *a=(allocation_t*)p-1;live-=a->size;free(a);}}
uint32_t esp_crc32_le(uint32_t crc,const uint8_t *data,uint32_t bytes)
{
    crc=~crc;
    while(bytes--){crc^=*data++;for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xEDB88320U:0);}
    return ~crc;
}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label)
{
    assert(!runtime && type==ESP_PARTITION_TYPE_DATA && subtype==ESP_PARTITION_SUBTYPE_ANY);
    assert(!strcmp(label,"lockanim"));return partition_present?&partition:NULL;
}
static void bounds(const esp_partition_t *p,size_t offset,size_t bytes)
{assert(!runtime && !sd_locked && p==&partition && offset<=p->size && bytes<=p->size-offset);}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *data,size_t bytes)
{
    bounds(p,offset,bytes);++reads;
    if(fail_read && reads==(unsigned)fail_read)return ESP_FAIL;
    memcpy(data,flash+offset,bytes);return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p,size_t offset,const void *data,size_t bytes)
{
    bounds(p,offset,bytes);++writes;
    bool fail=fail_write && writes==(unsigned)fail_write;
    size_t n=fail?bytes/2:bytes;
    const uint8_t *src=data;
    for(size_t i=0;i<n;++i){assert((flash[offset+i]&src[i])==src[i]);flash[offset+i]&=src[i];}
    if(corrupt_payload && offset%SLOT_BYTES>=SECTOR_BYTES){flash[offset+4]^=1;corrupt_payload=false;}
    return fail?ESP_FAIL:ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t bytes)
{
    bounds(p,offset,bytes);assert(!(offset%SECTOR_BYTES) && !(bytes%SECTOR_BYTES));++erases;
    bool fail=fail_erase && erases==(unsigned)fail_erase;
    memset(flash+offset,0xFF,fail?bytes/2:bytes);return fail?ESP_FAIL:ESP_OK;
}
bool sd_card_io_lock(TickType_t wait)
{(void)wait;assert(!runtime && !sd_locked);++sd_locks;sd_locked=true;return true;}
void sd_card_io_unlock(void){assert(sd_locked);sd_locked=false;}
bool sd_card_is_mounted(void){return mounted;}
bool sd_card_is_present(void){return present;}

static void boot(void)
{
    heap_caps_free(s_data);s_data=NULL;s_bytes=0;s_initialized=false;
    atomic_store(&s_ready,false);runtime=false;
    reads=writes=erases=sd_locks=0;fail_write=fail_erase=fail_read=0;
    corrupt_payload=fail_alloc=false;peak=0;
    assert(!sd_locked && !live);
}
static size_t make_frame(uint8_t *dst,size_t payload,uint8_t value)
{
    /* Embedded SOI/EOI inside APP1 must not split the outer frame. */
    static const uint8_t prefix[]={
        0xFF,0xD8,0xFF,0xE1,0,12,1,2,0xFF,0xD8,0xFF,0xD9,3,4,5,6,
        0xFF,0xC0,0,17,8,1,0xCC,1,0xCC,3,1,0x22,0,2,0x11,1,3,0x11,1,
        0xFF,0xDA,0,12,3,1,0,2,0x11,3,0x11,0,63,0};
    memcpy(dst,prefix,sizeof(prefix));memset(dst+sizeof(prefix),value,payload);
    size_t n=sizeof(prefix)+payload;
    dst[n++]=0xFF;dst[n++]=0;dst[n++]=0xFF;dst[n++]=0xD0;
    dst[n++]=0xFF;dst[n++]=0xD9;return n;
}
static void source_file(const uint8_t *data,size_t bytes)
{
    FILE *f=fopen(LOCK_ANIMATION_SD_PATH,"wb");assert(f);
    assert(fwrite(data,1,bytes,f)==bytes);assert(!fclose(f));
}
static void check_cache(const uint8_t *expected,size_t bytes,unsigned frames)
{
    assert(s_bytes==bytes && !memcmp(s_data,expected,bytes));
    unsigned before_reads=reads,before_sd=sd_locks,before_writes=writes;
    runtime=true;
    for(unsigned repeat=0;repeat<5;++repeat){
        size_t position=0,amount;const uint8_t *frame;unsigned count=0;
        while(lock_animation_next_frame(&position,&frame,&amount)){
            assert(frame[0]==0xFF && frame[1]==0xD8 && frame[amount-2]==0xFF && frame[amount-1]==0xD9);
            ++count;
        }
        assert(count==frames);
        position=SIZE_MAX;assert(!lock_animation_next_frame(&position,&frame,&amount));
    }
    assert(reads==before_reads && sd_locks==before_sd && writes==before_writes);
    assert(peak<=LOCK_ANIMATION_MAX_BYTES+IO_BYTES && live==bytes);
    runtime=false;
}
int main(void)
{
    MKDIR(SD_MOUNT_POINT);MKDIR(SD_SYS_DIR);MKDIR(SD_SYS_DIR "/MEDIA");MKDIR(SD_SYS_DIR "/MEDIA/lock_scr");
    remove(LOCK_ANIMATION_SD_PATH);memset(flash,0xFF,sizeof(flash));
    assert(esp_crc32_le(0,(const uint8_t*)"123456789",9)==0xCBF43926U);
    uint8_t *a=malloc(200000),*b=malloc(200000);assert(a && b);
    size_t a_bytes=make_frame(a,70000,0x23);a_bytes+=make_frame(a+a_bytes,20,0x24);
    size_t b_bytes=make_frame(b,70000,0x43);b_bytes+=make_frame(b+b_bytes,20,0x44);
    assert(a_bytes==b_bytes);
    boot();assert(lock_animation_init()==ESP_ERR_NOT_FOUND && !writes && !erases);
    size_t pos=0,n;const uint8_t *frame;assert(!lock_animation_next_frame(&pos,&frame,&n));
    FILE *fixture=fopen(ANIMATION_FIXTURE_DIR "/baseline-460.mjpeg","rb");assert(fixture);
    size_t fixture_bytes=fread(a,1,200000,fixture);assert(fixture_bytes && feof(fixture) && !fclose(fixture));
    source_file(a,fixture_bytes);boot();assert(lock_animation_init()==ESP_OK);check_cache(a,fixture_bytes,2);
    puts("PASS genuine FFmpeg baseline MJPEG import and complete frame iteration");
    boot();memset(flash,0xFF,sizeof(flash));
    a_bytes=make_frame(a,70000,0x23);a_bytes+=make_frame(a+a_bytes,20,0x24);
    source_file(a,a_bytes);assert(lock_animation_init()==ESP_OK);check_cache(a,a_bytes,2);
    assert(writes>0 && get32(flash)==HEADER_MAGIC && get32(flash+8)==1);
    uint8_t *baseline=malloc(sizeof(flash));assert(baseline);memcpy(baseline,flash,sizeof(flash));
    boot();assert(lock_animation_init()==ESP_OK && !writes && !erases);check_cache(a,a_bytes,2);
    assert(lock_animation_init()==ESP_ERR_INVALID_STATE);
    puts("PASS initial import, checksum identity avoids rewrites, single-init guard, replay performs no I/O");

    source_file(b,b_bytes);boot();assert(lock_animation_init()==ESP_OK);check_cache(b,b_bytes,2);
    assert(get32(flash+SLOT_BYTES+8)==2 && !memcmp(flash,baseline,SLOT_BYTES));
    unsigned good_writes=writes,good_erases=erases;
    remove(LOCK_ANIMATION_SD_PATH);mounted=false;present=false;
    boot();assert(lock_animation_init()==ESP_OK && !writes);check_cache(b,b_bytes,2);
    mounted=present=true;
    flash[SLOT_BYTES+SECTOR_BYTES+100]^=1;
    boot();assert(lock_animation_init()==ESP_OK && !writes);check_cache(a,a_bytes,2);
    puts("PASS same-size file replacement, no-card reboot persistence, corrupt newest slot falls back to older copy");

    uint8_t invalid[]={0xFF,0xD8,0xFF,0xD9};
    source_file(invalid,sizeof(invalid));boot();assert(lock_animation_init()==ESP_OK && !writes && !erases);check_cache(a,a_bytes,2);
    uint32_t count;
    size_t first_end;assert(frame_end(a,a_bytes,0,&first_end));
    for(size_t size=0;size<first_end;++size)assert(!validate_animation(a,size,&count));
    uint8_t small[1000];size_t small_size=make_frame(small,20,0x10);
    small[17]=0xC2;assert(!validate_animation(small,small_size,&count)); /* progressive */
    small[17]=0xC0;small[23]=0;assert(!validate_animation(small,small_size,&count)); /* dimensions */
    memset(small,0,sizeof(small));assert(!validate_animation(small,sizeof(small),&count));
    puts("PASS truncated headers/entropy, progressive and wrong dimensions rejected without erasing old animation");

    source_file(b,b_bytes);
    for(unsigned kind=0;kind<2;++kind){
        unsigned total=kind?good_erases:good_writes;
        for(unsigned fault=1;fault<=total;++fault){
            memcpy(flash,baseline,sizeof(flash));boot();
            if(kind)fail_erase=fault;else fail_write=fault;
            assert(lock_animation_init()==ESP_OK);check_cache(a,a_bytes,2);
            assert(!memcmp(flash,baseline,SLOT_BYTES));
            /* Boot after simulated torn write/erase with no source available. */
            mounted=false;boot();assert(lock_animation_init()==ESP_OK);check_cache(a,a_bytes,2);mounted=true;
        }
    }
    memcpy(flash,baseline,sizeof(flash));boot();corrupt_payload=true;
    assert(lock_animation_init()==ESP_OK);check_cache(a,a_bytes,2);
    assert(!memcmp(flash,baseline,SLOT_BYTES));
    memcpy(flash,baseline,sizeof(flash));boot();fail_read=1;
    assert(lock_animation_init()!=ESP_OK && !writes && !erases); /* unreadable state never overwritten */
    memcpy(flash,baseline,sizeof(flash));boot();partition_present=false;
    assert(lock_animation_init()==ESP_ERR_NOT_FOUND && !writes && !sd_locks);partition_present=true;
    boot();fail_alloc=true;assert(lock_animation_init()==ESP_ERR_NO_MEM && !writes && !erases);
    puts("PASS every write/erase interruption, payload readback corruption, partition/read/allocation errors preserve active slot");

    uint8_t *large=malloc(LOCK_ANIMATION_MAX_BYTES);assert(large);
    size_t overhead=make_frame(small,0,0x22);
    assert(make_frame(large,LOCK_ANIMATION_MAX_BYTES-overhead,0x22)==LOCK_ANIMATION_MAX_BYTES);
    memcpy(flash,baseline,sizeof(flash));source_file(large,LOCK_ANIMATION_MAX_BYTES);
    boot();assert(lock_animation_init()==ESP_OK);check_cache(large,LOCK_ANIMATION_MAX_BYTES,1);
    FILE *f=fopen(LOCK_ANIMATION_SD_PATH,"wb");assert(f);
    assert(!fseek(f,LOCK_ANIMATION_MAX_BYTES,SEEK_SET) && fputc(0,f)!=EOF && !fclose(f));
    boot();assert(lock_animation_init()==ESP_OK && !writes && !erases);check_cache(large,LOCK_ANIMATION_MAX_BYTES,1);
    puts("PASS exact 4MiB import, larger source rejected, PSRAM peak <= file limit + 16KiB scratch");
    remove(LOCK_ANIMATION_SD_PATH);boot();assert(!live);
    free(a);free(b);free(baseline);free(large);
    puts("ALL LOCK ANIMATION TESTS PASSED");return 0;
}
