#include "lock_animation.h"
#include "sd_card.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>

#define PARTITION_LABEL "lockanim"
#define HEADER_BYTES 64U
#define SECTOR_BYTES 4096U
#define SLOT_BYTES (SECTOR_BYTES + LOCK_ANIMATION_MAX_BYTES)
#define IO_BYTES (16U * 1024U)
#define HEADER_MAGIC 0x31414B4CU /* little endian: LKA1 */
#define FORMAT_VERSION 1U
static const char *TAG = "lock_animation";
static uint8_t *s_data;
static size_t s_bytes;
static atomic_bool s_ready;
static bool s_initialized;

typedef struct {
    bool valid;
    unsigned index;
    uint64_t sequence;
    uint32_t bytes, crc, frames;
} slot_t;

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t n)
{ for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(n>>(i*8)); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0]<<8 | p[1]; }

/* 只接受当前画布支持的460x460、8bit三分量baseline JPEG串流。
 * 按segment长度跳过APP/缩略图，再识别entropy中的转义/重启标记，不能
 * 直接搜索第一个FFD9，否则APP段内的缩略图会被误当成整帧结束。
 * 这是格式/边界检查，不声称能替代硬件逐帧解码或验证熵编码内容。 */
static bool frame_end(const uint8_t *data, size_t size, size_t start, size_t *end)
{
    if (start>size || size-start<4 || data[start]!=0xFF || data[start+1]!=0xD8) return false;
    size_t p=start+2;
    bool have_sof=false;
    while (p<size) {
        if (data[p++]!=0xFF) return false;
        while (p<size && data[p]==0xFF) ++p;
        if (p>=size) return false;
        unsigned marker=data[p++];
        if (!marker || marker==0xD8 || marker==0xD9 || (marker>=0xD0 && marker<=0xD7)) return false;
        if (size-p<2) return false;
        size_t length=be16(data+p);
        if (length<2 || length>size-p) return false;
        const uint8_t *body=data+p+2;
        if (marker>=0xC0 && marker<=0xCF && marker!=0xC4 && marker!=0xC8 && marker!=0xCC) {
            if (marker!=0xC0 || have_sof || length!=17 || body[0]!=8 ||
                be16(body+1)!=LOCK_ANIMATION_HEIGHT || be16(body+3)!=LOCK_ANIMATION_WIDTH ||
                body[5]!=3 || (body[7]!=0x11 && body[7]!=0x21 && body[7]!=0x22) ||
                body[10]!=0x11 || body[13]!=0x11) return false;
            have_sof=true;
        }
        p+=length;
        if (marker!=0xDA) continue;
        if (!have_sof || length!=12 || body[0]!=3 || body[7]!=0 || body[8]!=63 || body[9]!=0) return false;
        while (p<size) {
            if (data[p++]!=0xFF) continue;
            while (p<size && data[p]==0xFF) ++p;
            if (p>=size) return false;
            unsigned code=data[p++];
            if (!code || (code>=0xD0 && code<=0xD7)) continue;
            if (code==0xD9) { *end=p; return true; }
            return false;
        }
        return false;
    }
    return false;
}
static size_t skip_padding(const uint8_t *data, size_t size, size_t p)
{
    while (p<size && (data[p]==0 || data[p]==' ' || data[p]=='\r' || data[p]=='\n' || data[p]=='\t')) ++p;
    return p;
}
static bool validate_animation(const uint8_t *data, size_t size, uint32_t *frames)
{
    size_t p=0, end;
    *frames=0;
    if (!size || size>LOCK_ANIMATION_MAX_BYTES) return false;
    while ((p=skip_padding(data,size,p))<size) {
        if (!frame_end(data,size,p,&end)) return false;
        ++*frames; p=end;
    }
    return *frames!=0;
}

static esp_err_t flash_crc(const esp_partition_t *part, size_t offset, size_t bytes,
                           uint8_t *scratch, uint32_t *crc)
{
    *crc=0;
    while (bytes) {
        size_t n=bytes<IO_BYTES ? bytes : IO_BYTES;
        esp_err_t ret=esp_partition_read(part,offset,scratch,n);
        if (ret!=ESP_OK) return ret;
        *crc=esp_crc32_le(*crc,scratch,n);
        offset+=n; bytes-=n;
    }
    return ESP_OK;
}
static void encode_header(uint8_t *h, const slot_t *slot)
{
    memset(h,0,HEADER_BYTES);
    put32(h,HEADER_MAGIC); put32(h+4,FORMAT_VERSION);
    put32(h+8,(uint32_t)slot->sequence); put32(h+12,(uint32_t)(slot->sequence>>32));
    put32(h+16,slot->bytes); put32(h+20,slot->crc); put32(h+24,slot->frames);
    put32(h+28,LOCK_ANIMATION_WIDTH); put32(h+32,LOCK_ANIMATION_HEIGHT);
    put32(h+36,LOCK_ANIMATION_FPS);
    put32(h+60,esp_crc32_le(0,h,60));
}
static esp_err_t inspect_slot(const esp_partition_t *part, unsigned index,
                              uint8_t *scratch, slot_t *slot)
{
    uint8_t h[HEADER_BYTES];
    *slot=(slot_t){.index=index};
    esp_err_t ret=esp_partition_read(part,index*SLOT_BYTES,h,sizeof(h));
    if (ret!=ESP_OK) return ret;
    if (get32(h)!=HEADER_MAGIC || get32(h+4)!=FORMAT_VERSION ||
        get32(h+60)!=esp_crc32_le(0,h,60) || get32(h+28)!=LOCK_ANIMATION_WIDTH ||
        get32(h+32)!=LOCK_ANIMATION_HEIGHT || get32(h+36)!=LOCK_ANIMATION_FPS)
        return ESP_ERR_NOT_FOUND;
    slot->sequence=get32(h+8) | (uint64_t)get32(h+12)<<32;
    slot->bytes=get32(h+16); slot->crc=get32(h+20); slot->frames=get32(h+24);
    if (!slot->sequence || !slot->bytes || slot->bytes>LOCK_ANIMATION_MAX_BYTES ||
        !slot->frames || slot->frames>slot->bytes/4) return ESP_ERR_NOT_FOUND;
    uint32_t crc;
    ret=flash_crc(part,index*SLOT_BYTES+SECTOR_BYTES,slot->bytes,scratch,&crc);
    if (ret!=ESP_OK) return ret;
    if (crc!=slot->crc) return ESP_ERR_NOT_FOUND;
    slot->valid=true;
    return ESP_OK;
}

/* SD文件只在启动导入时打开，整个FILE生命周期由已有SD锁保护。
 * 读完即关闭/解锁，随后校验、Flash擦写均不持SD锁，也不持LVGL锁。
 * 单份压缩文件最大4MiB；失败不保留FILE，不切换到内部SRAM分配。 */
static esp_err_t read_source(uint8_t **data, size_t *bytes)
{
    *data=NULL; *bytes=0;
    if (!sd_card_io_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    esp_err_t ret=ESP_ERR_NOT_FOUND;
    FILE *fp=NULL;
    if (!sd_card_is_mounted() || !sd_card_is_present()) goto done;
    fp=fopen(LOCK_ANIMATION_SD_PATH,"rb");
    if (!fp) goto done;
    ret=ESP_ERR_INVALID_SIZE;
    if (fseek(fp,0,SEEK_END)!=0) goto done;
    long length=ftell(fp);
    if (length<=0 || (unsigned long)length>LOCK_ANIMATION_MAX_BYTES || fseek(fp,0,SEEK_SET)!=0) goto done;
    *data=heap_caps_malloc((size_t)length,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!*data) { ret=ESP_ERR_NO_MEM; goto done; }
    ret=ESP_FAIL;
    if (fread(*data,1,(size_t)length,fp)!=(size_t)length || ferror(fp) || !sd_card_is_present()) goto done;
    *bytes=(size_t)length;
    ret=ESP_OK;
done:
    if (fp) fclose(fp);
    sd_card_io_unlock();
    if (ret!=ESP_OK) { heap_caps_free(*data); *data=NULL; }
    return ret;
}

/* 事务只写非活动槽：先清除该槽旧header，再写payload并读回CRC，最后写
 * 含CRC的header作为提交点。旧活动槽在全部步骤中保持原样；掉电导致的
 * 不完整新槽在下次启动被忽略。这里不承诺异常供电下的物理Flash可靠性。 */
static esp_err_t store_source(const esp_partition_t *part, const slot_t *current,
                              const uint8_t *data, size_t bytes, uint32_t frames,
                              uint32_t crc, uint8_t *scratch)
{
    if (current && current->sequence==UINT64_MAX) return ESP_ERR_INVALID_STATE;
    slot_t next={.index=current ? 1U-current->index : 0,
                 .sequence=current ? current->sequence+1 : 1,
                 .bytes=(uint32_t)bytes,.crc=crc,.frames=frames};
    size_t base=next.index*SLOT_BYTES;
    esp_err_t ret=esp_partition_erase_range(part,base,SECTOR_BYTES);
    if (ret!=ESP_OK) return ret;
    size_t erase=(bytes+SECTOR_BYTES-1)&~(size_t)(SECTOR_BYTES-1);
    /* 分批擦写，避免一次提出整个分区请求；所有范围局限于新槽。 */
    size_t offset=0;
    while (offset<erase) {
        size_t n=erase-offset;
        if (n>64U*1024U) n=64U*1024U;
        ret=esp_partition_erase_range(part,base+SECTOR_BYTES+offset,n);
        if (ret!=ESP_OK) return ret;
        offset+=n;
    }
    offset=0;
    while (offset<bytes) {
        size_t n=bytes-offset;
        if (n>IO_BYTES) n=IO_BYTES;
        ret=esp_partition_write(part,base+SECTOR_BYTES+offset,data+offset,n);
        if (ret!=ESP_OK) return ret;
        offset+=n;
    }
    uint32_t written_crc;
    ret=flash_crc(part,base+SECTOR_BYTES,bytes,scratch,&written_crc);
    if (ret!=ESP_OK) return ret;
    if (written_crc!=crc) return ESP_ERR_INVALID_RESPONSE;
    uint8_t h[HEADER_BYTES], readback[HEADER_BYTES];
    encode_header(h,&next);
    ret=esp_partition_write(part,base,h,sizeof(h));
    if (ret!=ESP_OK) return ret;
    ret=esp_partition_read(part,base,readback,sizeof(readback));
    if (ret!=ESP_OK) return ret;
    if (memcmp(h,readback,sizeof(h))) return ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG,"Imported animation: slot=%u, sequence=%" PRIu64 ", bytes=%u, frames=%u",
             next.index,next.sequence,next.bytes,next.frames);
    return ESP_OK;
}
static esp_err_t load_slot(const esp_partition_t *part, const slot_t *slot)
{
    s_data=heap_caps_malloc(slot->bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_data) return ESP_ERR_NO_MEM;
    esp_err_t ret=esp_partition_read(part,slot->index*SLOT_BYTES+SECTOR_BYTES,s_data,slot->bytes);
    uint32_t frames=0;
    if (ret==ESP_OK && (esp_crc32_le(0,s_data,slot->bytes)!=slot->crc ||
        !validate_animation(s_data,slot->bytes,&frames) || frames!=slot->frames)) ret=ESP_ERR_INVALID_RESPONSE;
    if (ret!=ESP_OK) { heap_caps_free(s_data); s_data=NULL; return ret; }
    s_bytes=slot->bytes;
    ESP_LOGI(TAG,"Loaded Flash animation: slot=%u, bytes=%u, frames=%u",slot->index,slot->bytes,frames);
    return ESP_OK;
}

esp_err_t lock_animation_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized=true;
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_ANY,PARTITION_LABEL);
    if (!part) { ESP_LOGE(TAG,"Missing lockanim partition; flash the new partition table"); return ESP_ERR_NOT_FOUND; }
    if (part->size<2U*SLOT_BYTES || part->erase_size!=SECTOR_BYTES || part->readonly || part->encrypted)
        return ESP_ERR_NOT_SUPPORTED;
    uint8_t *scratch=heap_caps_malloc(IO_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!scratch) return ESP_ERR_NO_MEM;
    slot_t slots[2];
    bool read_failed=false;
    for (unsigned i=0; i<2; ++i) {
        esp_err_t r=inspect_slot(part,i,scratch,&slots[i]);
        if (r!=ESP_OK && r!=ESP_ERR_NOT_FOUND) read_failed=true;
    }
    slot_t *current=NULL;
    for (unsigned i=0; i<2; ++i)
        if (slots[i].valid && (!current || slots[i].sequence>current->sequence)) current=&slots[i];
    uint8_t *source=NULL;
    size_t bytes=0;
    esp_err_t ret=read_failed ? ESP_FAIL : read_source(&source,&bytes);
    if (ret==ESP_OK) {
        uint32_t frames=0;
        if (!validate_animation(source,bytes,&frames)) ret=ESP_ERR_INVALID_RESPONSE;
        else {
            uint32_t crc=esp_crc32_le(0,source,bytes);
            if (current && current->bytes==bytes && current->crc==crc) {
                ESP_LOGI(TAG,"Animation unchanged; no Flash write");
            } else ret=store_source(part,current,source,bytes,frames,crc,scratch);
        }
        if (ret==ESP_OK) { s_data=source; s_bytes=bytes; source=NULL; }
    }
    heap_caps_free(source);
    if (ret!=ESP_OK && ret!=ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG,"Animation import unavailable (%s); retaining verified Flash copy",esp_err_to_name(ret));
    if (!s_data && current) {
        ret=load_slot(part,current);
        slot_t *older=&slots[1U-current->index];
        if (ret!=ESP_OK && older->valid) ret=load_slot(part,older);
    }
    heap_caps_free(scratch);
    if (!s_data) return ret==ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    atomic_store_explicit(&s_ready,true,memory_order_release);
    return ESP_OK;
}

bool lock_animation_next_frame(size_t *position, const uint8_t **frame, size_t *bytes)
{
    if (!position || !frame || !bytes || !atomic_load_explicit(&s_ready,memory_order_acquire)) return false;
    size_t p=*position, end;
    if (p>s_bytes) return false;
    p=skip_padding(s_data,s_bytes,p);
    if (!frame_end(s_data,s_bytes,p,&end)) return false;
    *frame=s_data+p; *bytes=end-p; *position=end;
    return true;
}
