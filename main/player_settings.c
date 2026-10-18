#include "player_settings.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

/* 一个 blob：版本头 + TLV 字段 + CRC32。每个长度都先验证再复制；
 * 总空间固定 1KiB，不随曲库规模增加。新增同版本可选字段用新 ID，
 * 旧版会跳过未知 ID；不兼容格式须升级版本并显式迁移，禁止覆盖未知新版。
 * 不保存播放毫秒位置；后续需要时分配独立 ID，不能改变已有 ID 的含义。 */
#define SETTINGS_MAX 1024
#define SETTINGS_HEADER 12
#define SETTINGS_VERSION 1
static player_settings_t s_saved = {.brightness = 80, .volume = 50};
static player_settings_t s_ram = {.brightness = 80, .volume = 50};
static bool s_ready, s_have_saved;
static uint8_t s_encoded[SETTINGS_MAX]; /* 仅 init/电源 worker 串行使用 */

static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = ~0U;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1)));
    }
    return ~c;
}
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0; i<4; ++i) p[i] = v >> (i*8); }
static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static size_t add_field(uint8_t *b, size_t n, uint8_t id, const void *v, uint16_t len)
{
    b[n++] = id; put16(b+n, len); n += 2; memcpy(b+n, v, len); return n+len;
}
static size_t encode(const player_settings_t *v, uint8_t *b)
{
    memcpy(b, "HMP2", 4); put16(b+4, SETTINGS_VERSION);
    size_t n = SETTINGS_HEADER;
    uint8_t muted=v->muted, automatic=v->auto_brightness;
    n=add_field(b,n,1,&v->brightness,1); n=add_field(b,n,2,&v->volume,1);
    n=add_field(b,n,3,&muted,1); n=add_field(b,n,4,&automatic,1);
    n=add_field(b,n,5,v->song_folder,(uint16_t)strlen(v->song_folder));
    n=add_field(b,n,6,v->song_audio,(uint16_t)strlen(v->song_audio));
    put16(b+6,n); put32(b+8,crc32(b+SETTINGS_HEADER,n-SETTINGS_HEADER));
    return n;
}
static esp_err_t decode(const uint8_t *b, size_t n, player_settings_t *v)
{
    if (n<SETTINGS_HEADER || n>SETTINGS_MAX || memcmp(b,"HMP2",4) || get16(b+6)!=n)
        return ESP_ERR_INVALID_SIZE;
    if (get16(b+4)!=SETTINGS_VERSION) return ESP_ERR_NOT_SUPPORTED;
    if (get32(b+8)!=crc32(b+SETTINGS_HEADER,n-SETTINGS_HEADER)) return ESP_ERR_INVALID_RESPONSE;
    player_settings_t tmp = {.brightness=80,.volume=50};
    unsigned seen=0;
    for (size_t off=SETTINGS_HEADER; off<n;) {
        if (n-off<3) return ESP_ERR_INVALID_SIZE;
        uint8_t id=b[off++]; uint16_t len=get16(b+off); off+=2;
        if (len>n-off) return ESP_ERR_INVALID_SIZE;
        if (id>=1 && id<=6) {
            if (seen & (1U<<id)) return ESP_ERR_INVALID_RESPONSE;
            seen |= 1U<<id;
        }
        if (id>=1 && id<=4) {
            if (len!=1) return ESP_ERR_INVALID_SIZE;
            uint8_t value=b[off];
            if ((id==1 && (value<5 || value>100)) || (id==2 && value>100) ||
                (id>=3 && value>1)) return ESP_ERR_INVALID_ARG;
            if (id==1) tmp.brightness=value;
            if (id==2) tmp.volume=value;
            if (id==3) tmp.muted=value;
            if (id==4) tmp.auto_brightness=value;
        } else if (id==5 || id==6) {
            if (len>=MUSIC_NAME_MAX || memchr(b+off,0,len)) return ESP_ERR_INVALID_SIZE;
            char *dest=id==5?tmp.song_folder:tmp.song_audio;
            memcpy(dest,b+off,len); dest[len]=0;
        }
        off+=len;
    }
    *v=tmp; return ESP_OK;
}

esp_err_t player_settings_init(void)
{
    esp_err_t ret=nvs_flash_init();
    if (ret!=ESP_OK) return ret; /* 不套用示例中的全分区擦除重试。 */
    nvs_handle_t handle;
    ret=nvs_open("player",NVS_READWRITE,&handle);
    if (ret!=ESP_OK) return ret;
    size_t len=0;
    ret=nvs_get_blob(handle,"settings",NULL,&len);
    if (ret==ESP_ERR_NVS_NOT_FOUND) { s_ready=true; ret=ESP_OK; }
    else if (ret==ESP_OK) {
        if (len>sizeof(s_encoded)) ret=ESP_ERR_INVALID_SIZE;
        else {
            ret=nvs_get_blob(handle,"settings",s_encoded,&len);
            if (ret==ESP_OK) ret=decode(s_encoded,len,&s_ram);
            if (ret==ESP_OK) { s_saved=s_ram; s_have_saved=true; s_ready=true; }
        }
    }
    nvs_close(handle);
    /* 损坏/新版数据保持原样，本次启动使用默认值且禁写，留给后续迁移。 */
    return ret;
}
void player_settings_restore_ui(void)
{
    ui_device_state_t *st=ui_get_device_state();
    st->brightness=s_ram.brightness; st->volume=s_ram.volume;
    st->muted=s_ram.muted; st->auto_brightness=s_ram.auto_brightness;
}
void player_settings_capture(player_settings_t *out)
{
    ui_device_state_t *st=ui_get_device_state();
    s_ram.brightness=st->brightness<5?5:st->brightness;
    s_ram.volume=st->volume; s_ram.muted=st->muted; s_ram.auto_brightness=st->auto_brightness;
    *out=s_ram;
}
void player_settings_get_song(char *folder, char *audio)
{ memcpy(folder,s_ram.song_folder,MUSIC_NAME_MAX); memcpy(audio,s_ram.song_audio,MUSIC_NAME_MAX); }
void player_settings_set_song(const music_item_t *item)
{
    memcpy(s_ram.song_folder,item->name,MUSIC_NAME_MAX);
    memcpy(s_ram.song_audio,item->audio,MUSIC_NAME_MAX);
}
bool player_settings_equal(const player_settings_t *a, const player_settings_t *b)
{
    return a->brightness==b->brightness && a->volume==b->volume &&
        a->muted==b->muted && a->auto_brightness==b->auto_brightness &&
        !strcmp(a->song_folder,b->song_folder) && !strcmp(a->song_audio,b->song_audio);
}
esp_err_t player_settings_save(const player_settings_t *v)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (v->brightness<5 || v->brightness>100 || v->volume>100 ||
        !memchr(v->song_folder,0,MUSIC_NAME_MAX) || !memchr(v->song_audio,0,MUSIC_NAME_MAX))
        return ESP_ERR_INVALID_ARG;
    if (s_have_saved && player_settings_equal(v,&s_saved))
        return ESP_OK;
    nvs_handle_t handle;
    esp_err_t ret=nvs_open("player",NVS_READWRITE,&handle);
    if (ret!=ESP_OK) return ret;
    size_t len=encode(v,s_encoded);
    ret=nvs_set_blob(handle,"settings",s_encoded,len);
    if (ret==ESP_OK) ret=nvs_commit(handle);
    /* 关机前读回整份快照，不能只凭 set_blob 返回就宣称保存成功。 */
    if (ret==ESP_OK) {
        nvs_close(handle);
        ret=nvs_open("player",NVS_READONLY,&handle);
        if (ret!=ESP_OK) return ret;
        size_t got=sizeof(s_encoded); player_settings_t readback;
        ret=nvs_get_blob(handle,"settings",s_encoded,&got);
        if (ret==ESP_OK) ret=decode(s_encoded,got,&readback);
        if (ret==ESP_OK && (got!=len || !player_settings_equal(&readback,v)))
            ret=ESP_ERR_INVALID_RESPONSE;
    }
    nvs_close(handle);
    if (ret==ESP_OK) { s_saved=*v; s_have_saved=true; }
    return ret;
}
