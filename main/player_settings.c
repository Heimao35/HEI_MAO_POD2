#include "player_settings.h"
#include "ui/ui_motion_settings.h"
#include "ui/ui_lock_wallpaper_settings.h"
#include "bt_audio.h"
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
static void put64(uint8_t *p, uint64_t v)
{ for (unsigned i=0; i<8; ++i) p[i] = (uint8_t)(v >> (i*8)); }
static uint64_t get64(const uint8_t *p)
{
    uint64_t v=0;
    for (unsigned i=0; i<8; ++i) v |= (uint64_t)p[i] << (i*8);
    return v;
}
static size_t add_field(uint8_t *b, size_t n, uint8_t id, const void *v, uint16_t len)
{
    b[n++] = id; put16(b+n, len); n += 2; memcpy(b+n, v, len); return n+len;
}
static size_t encode(const player_settings_t *v, uint8_t *b)
{
    memcpy(b, "HMP2", 4); put16(b+4, SETTINGS_VERSION);
    size_t n = SETTINGS_HEADER;
    uint8_t muted=v->muted, automatic=v->auto_brightness;
    uint8_t motion_steps=v->motion_steps, motion_wake=v->motion_wake;
    uint8_t anti_touch=v->anti_touch;
    n=add_field(b,n,1,&v->brightness,1); n=add_field(b,n,2,&v->volume,1);
    n=add_field(b,n,3,&muted,1); n=add_field(b,n,4,&automatic,1);
    n=add_field(b,n,5,v->song_folder,(uint16_t)strlen(v->song_folder));
    n=add_field(b,n,6,v->song_audio,(uint16_t)strlen(v->song_audio));
    /* 7/8 为新增的可选单字节开关字段。旧固件按未知 ID 跳过，因此不需要
     * 升级版本；旧数据缺少这两个字段时解码默认值保持关闭。 */
    n=add_field(b,n,7,&motion_steps,1); n=add_field(b,n,8,&motion_wake,1);
    /* 11 为新增的可选防误触开关（单字节 0/1）。与 7/8 相同，旧固件按
     * 未知 ID 跳过；旧数据缺少该字段时默认关闭（熄屏触摸仍可唤醒），
     * 与升级前的行为一致，不需要升级版本号。 */
    n=add_field(b,n,11,&anti_touch,1);
    /* 9 为可选配对记录：[valid][bda 6][name<=32]。未配对时不写该字段，
     * 旧固件按未知 ID 跳过，因此同样不需要升级版本。 */
    if (v->bt_paired) {
        uint8_t pair[1+6+PLAYER_SETTINGS_BT_NAME_MAX];
        size_t pair_len=0;
        pair[pair_len++]=1;
        memcpy(pair+pair_len,v->bt_paired_bda,6); pair_len+=6;
        size_t name_len=strnlen(v->bt_paired_name,sizeof(v->bt_paired_name));
        memcpy(pair+pair_len,v->bt_paired_name,name_len); pair_len+=name_len;
        n=add_field(b,n,9,pair,(uint16_t)pair_len);
    }
    /* 12 为最近一次 U4 FW_INFO 的版本字符串（不含 NUL）。只用于设置页
     * 显示和 U4 更新比较；旧固件按未知 ID 跳过，版本号不变。 */
    size_t u4_len = strnlen(v->u4_version, sizeof(v->u4_version));
    if (u4_len) n=add_field(b,n,12,v->u4_version,(uint16_t)u4_len);
    /* 10 为可选阅读进度表：[count 1][key 8][page 4][pages 4][stamp 4]...。
     * 只保存最近 16 本；旧固件按未知 ID 跳过，版本号不变。 */
    uint8_t ebook_count = v->ebook_count > PLAYER_SETTINGS_EBOOK_MAX
                              ? PLAYER_SETTINGS_EBOOK_MAX : v->ebook_count;
    if (ebook_count) {
        uint8_t table[1 + PLAYER_SETTINGS_EBOOK_MAX * 20];
        size_t tn = 0;
        table[tn++] = ebook_count;
        for (unsigned i = 0; i < ebook_count; ++i) {
            put64(table+tn, v->ebook[i].key); tn += 8;
            put32(table+tn, v->ebook[i].page); tn += 4;
            put32(table+tn, v->ebook[i].pages); tn += 4;
            put32(table+tn, v->ebook[i].stamp); tn += 4;
        }
        n=add_field(b,n,10,table,(uint16_t)tn);
    }
    put16(b+6,n); put32(b+8,crc32(b+SETTINGS_HEADER,n-SETTINGS_HEADER));
    return n;
}
static esp_err_t decode(const uint8_t *b, size_t n, player_settings_t *v)
{
    if (n<SETTINGS_HEADER || n>SETTINGS_MAX || memcmp(b,"HMP2",4) || get16(b+6)!=n)
        return ESP_ERR_INVALID_SIZE;
    if (get16(b+4)!=SETTINGS_VERSION) return ESP_ERR_NOT_SUPPORTED;
    if (get32(b+8)!=crc32(b+SETTINGS_HEADER,n-SETTINGS_HEADER)) return ESP_ERR_INVALID_RESPONSE;
    player_settings_t tmp = {.brightness=80,.volume=50,.anti_touch=false};
    unsigned seen=0;
    for (size_t off=SETTINGS_HEADER; off<n;) {
        if (n-off<3) return ESP_ERR_INVALID_SIZE;
        uint8_t id=b[off++]; uint16_t len=get16(b+off); off+=2;
        if (len>n-off) return ESP_ERR_INVALID_SIZE;
        if (id>=1 && id<=12) {
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
        } else if (id==7 || id==8) {
            /* 姿态开关只接受单字节 0/1，避免损坏数据把任意值当成真。 */
            if (len!=1) return ESP_ERR_INVALID_SIZE;
            if (b[off]>1) return ESP_ERR_INVALID_ARG;
            if (id==7) tmp.motion_steps=b[off]; else tmp.motion_wake=b[off];
        } else if (id==11) {
            /* 防误触开关同样只接受单字节 0/1；缺少字段时保持默认关闭。 */
            if (len!=1) return ESP_ERR_INVALID_SIZE;
            if (b[off]>1) return ESP_ERR_INVALID_ARG;
            tmp.anti_touch=b[off];
        } else if (id==12) {
            /* U4 版本：只接受长度合法且无内嵌 NUL 的 ASCII 字符串。 */
            if (len >= sizeof(tmp.u4_version) || memchr(b+off,0,len))
                return ESP_ERR_INVALID_SIZE;
            memcpy(tmp.u4_version,b+off,len);
            tmp.u4_version[len]=0;
        } else if (id==9) {
            /* 配对记录：valid + 6 字节 BDA + 至多 32 字节名称。名称必须
             * 以 TLV 长度结束，不接受内嵌 NUL；全 0 BDA 视为损坏。 */
            if (len<7 || len>1+6+PLAYER_SETTINGS_BT_NAME_MAX-1)
                return ESP_ERR_INVALID_SIZE;
            if (b[off]>1) return ESP_ERR_INVALID_ARG;
            if (!b[off]) {
                if (len!=7) return ESP_ERR_INVALID_SIZE;
            } else {
                uint8_t any=0;
                for (unsigned k=0;k<6;++k) any|=(uint8_t)b[off+1+k];
                if (!any) return ESP_ERR_INVALID_ARG;
                size_t name_len=(size_t)len-7;
                if (memchr(b+off+7,0,name_len)) return ESP_ERR_INVALID_SIZE;
                tmp.bt_paired=true;
                memcpy(tmp.bt_paired_bda,b+off+1,6);
                memcpy(tmp.bt_paired_name,b+off+7,name_len);
                tmp.bt_paired_name[name_len]=0;
            }
        } else if (id==10) {
            /* 阅读进度：count + count×20B。拒绝越界页码、零 key、重复 key
             * 和超过上限的总页数，损坏数据不能让 UI 跳到错误页面。 */
            if (len < 1) return ESP_ERR_INVALID_SIZE;
            uint8_t count=b[off];
            if (count > PLAYER_SETTINGS_EBOOK_MAX ||
                len != 1U + (size_t)count * 20U)
                return ESP_ERR_INVALID_SIZE;
            tmp.ebook_count=count;
            for (unsigned i=0;i<count;++i) {
                const uint8_t *p=b+off+1+i*20U;
                player_settings_ebook_t e;
                e.key=get64(p); e.page=get32(p+8);
                e.pages=get32(p+12); e.stamp=get32(p+16);
                if (!e.key || !e.pages ||
                    e.pages>PLAYER_SETTINGS_EBOOK_MAX_PAGES ||
                    e.page>=e.pages)
                    return ESP_ERR_INVALID_ARG;
                for (unsigned j=0;j<i;++j)
                    if (tmp.ebook[j].key==e.key) return ESP_ERR_INVALID_ARG;
                tmp.ebook[i]=e;
            }
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
    /* U4 更新路径会在 PMIC 重启前单独写 player/u4ver，避免丢失显示值。
     * 它优先于设置 blob 中的同名字段；损坏/超长值忽略。 */
    char u4ver[PLAYER_SETTINGS_U4_VERSION_MAX];
    size_t u4len = sizeof(u4ver);
    if (nvs_get_str(handle, "u4ver", u4ver, &u4len) == ESP_OK && u4ver[0]) {
        size_t n = strnlen(u4ver, sizeof(u4ver) - 1);
        memcpy(s_ram.u4_version, u4ver, n);
        s_ram.u4_version[n] = 0;
        if (s_have_saved) {
            memcpy(s_saved.u4_version, s_ram.u4_version, sizeof(s_saved.u4_version));
        }
    }
    nvs_close(handle);
    /* 损坏/新版数据保持原样，本次启动使用默认值且禁写，留给后续迁移。 */
    return ret;
}

esp_err_t player_settings_save_u4_version(const char *version)
{
    if (!s_ready || !version) return ESP_ERR_INVALID_STATE;
    /* 只写独立的小 key，不触碰 s_ram/s_saved：本函数可能在固件更新任务
     * 中调用，而电源 worker 可能正在做自己的快照。当前开机内的显示值由
     * bt_audio 缓存提供；下次开机由 player_settings_init 读回本 key。 */
    char bounded[PLAYER_SETTINGS_U4_VERSION_MAX];
    size_t n = strnlen(version, sizeof(bounded) - 1);
    memcpy(bounded, version, n);
    bounded[n] = '\0';
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("player", NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(handle, "u4ver", bounded);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}
void player_settings_restore_ui(void)
{
    ui_device_state_t *st=ui_get_device_state();
    st->brightness=s_ram.brightness; st->manual_brightness=s_ram.brightness; st->volume=s_ram.volume;
    st->muted=s_ram.muted; st->auto_brightness=s_ram.auto_brightness;
}
void player_settings_get_motion(bool *steps, bool *wake)
{
    /* 只读 RAM 快照；调用方随后把值套用到姿态页开关。 */
    if (steps) *steps=s_ram.motion_steps;
    if (wake) *wake=s_ram.motion_wake;
}
void player_settings_get_anti_touch(bool *anti_touch)
{
    /* 只读 RAM 快照；调用方随后把它套用到“锁屏和壁纸”开关和
     * screen_power 的触摸唤醒允许标志。 */
    if (anti_touch) *anti_touch=s_ram.anti_touch;
}
void player_settings_get_bluetooth(bool *valid, uint8_t bda[6],
                                   char *name, size_t name_size)
{
    /* 只读 RAM 快照；不访问 Flash/蓝牙链路。 */
    if (valid) *valid=s_ram.bt_paired;
    if (bda) memcpy(bda,s_ram.bt_paired_bda,6);
    if (name && name_size) {
        size_t len=strnlen(s_ram.bt_paired_name,sizeof(s_ram.bt_paired_name));
        if (len>=name_size) len=name_size-1;
        memcpy(name,s_ram.bt_paired_name,len); name[len]=0;
    }
}
void player_settings_get_u4_version(char *version, size_t version_size)
{
    if (!version || !version_size) return;
    size_t len=strnlen(s_ram.u4_version,sizeof(s_ram.u4_version));
    if (len>=version_size) len=version_size-1;
    memcpy(version,s_ram.u4_version,len); version[len]=0;
}
void player_settings_capture(player_settings_t *out)
{
    ui_device_state_t *st=ui_get_device_state();
    /* TLV字段1延续原格式，含义为用户手动亮度。自动亮度只更新brightness，
     * 不随环境变化保存，从而避免每次亮度调整后都触发3秒合并写入。
     * 开机先用手动值亮屏，自动模式恢复后再由新鲜ADC数据平滑接管。 */
    s_ram.brightness=st->manual_brightness<5?5:st->manual_brightness;
    s_ram.volume=st->volume; s_ram.muted=st->muted; s_ram.auto_brightness=st->auto_brightness;
    /* 姿态开关由设置子页维护用户意图，这里在现有 LVGL 锁快照中一并读取：
     * 不新增任务/定时器，也不让 UI 直接访问 Flash；开关变化会像音量/亮度
     * 一样进入电源 worker 的 3 秒合并保存。UI 尚未创建时取到默认关闭，
     * 但生产路径中 capture 只在 ui_manager_init() 之后才被调用。 */
    s_ram.motion_steps=ui_motion_settings_steps_enabled();
    s_ram.motion_wake=ui_motion_settings_wake_enabled();
    /* 防误触开关由“锁屏和壁纸”子页维护；与姿态开关相同，在电源
     * worker 的既有 LVGL 锁快照中读取，3 秒合并后写入 NVS。 */
    s_ram.anti_touch=ui_lock_wallpaper_settings_anti_touch_enabled();
    /* 配对记录由链路 worker 在 A2DP 连接成功后发布；这里只取 RAM 快照。
     * 未配对时 get_paired 会清零输出，s_ram 中不会残留旧地址。 */
    s_ram.bt_paired=bt_audio_get_paired(s_ram.bt_paired_bda,
                                        s_ram.bt_paired_name,
                                        sizeof(s_ram.bt_paired_name));
    /* U4 运行版本由链路 worker 在 FW_INFO 事件中更新；这里只取 RAM 快照。 */
    if (!bt_audio_get_u4_version(s_ram.u4_version, sizeof(s_ram.u4_version)))
        s_ram.u4_version[0]=0;
    *out=s_ram;
}
void player_settings_get_song(char *folder, char *audio)
{ memcpy(folder,s_ram.song_folder,MUSIC_NAME_MAX); memcpy(audio,s_ram.song_audio,MUSIC_NAME_MAX); }
void player_settings_set_song(const music_item_t *item)
{
    memcpy(s_ram.song_folder,item->name,MUSIC_NAME_MAX);
    memcpy(s_ram.song_audio,item->audio,MUSIC_NAME_MAX);
}
bool player_settings_get_ebook_progress(uint64_t key, uint32_t *page,
                                        uint32_t *pages)
{
    if (!key || !page || !pages) return false;
    unsigned count = s_ram.ebook_count;
    if (count > PLAYER_SETTINGS_EBOOK_MAX) count = PLAYER_SETTINGS_EBOOK_MAX;
    for (unsigned i=0;i<count;++i) {
        const player_settings_ebook_t *e=&s_ram.ebook[i];
        if (e->key==key && e->pages && e->page<e->pages) {
            *page=e->page; *pages=e->pages;
            return true;
        }
    }
    return false;
}
void player_settings_set_ebook_progress(uint64_t key, uint32_t page,
                                        uint32_t pages)
{
    if (!key || !pages || pages>PLAYER_SETTINGS_EBOOK_MAX_PAGES ||
        page>=pages)
        return;
    unsigned count = s_ram.ebook_count;
    if (count > PLAYER_SETTINGS_EBOOK_MAX) count = PLAYER_SETTINGS_EBOOK_MAX;
    uint32_t max_stamp=0;
    int found=-1;
    for (unsigned i=0;i<count;++i) {
        if (s_ram.ebook[i].stamp > max_stamp) max_stamp=s_ram.ebook[i].stamp;
        if (s_ram.ebook[i].key==key) found=(int)i;
    }
    /* 计数回绕时清零后重来；LRU 只用于淘汰，不影响正确性。 */
    if (max_stamp==UINT32_MAX) {
        for (unsigned i=0;i<count;++i) s_ram.ebook[i].stamp=0;
        max_stamp=0;
    }
    if (found<0) {
        if (count < PLAYER_SETTINGS_EBOOK_MAX) {
            found=(int)count;
            s_ram.ebook_count=(uint8_t)(count+1);
        } else {
            found=0;
            for (unsigned i=1;i<count;++i)
                if (s_ram.ebook[i].stamp < s_ram.ebook[found].stamp)
                    found=(int)i;
        }
    }
    s_ram.ebook[found].key=key;
    s_ram.ebook[found].page=page;
    s_ram.ebook[found].pages=pages;
    s_ram.ebook[found].stamp=max_stamp+1;
}
bool player_settings_equal(const player_settings_t *a, const player_settings_t *b)
{
    if (!(a->brightness==b->brightness && a->volume==b->volume &&
          a->muted==b->muted && a->auto_brightness==b->auto_brightness &&
          a->motion_steps==b->motion_steps && a->motion_wake==b->motion_wake &&
          a->anti_touch==b->anti_touch &&
          a->bt_paired==b->bt_paired &&
          !memcmp(a->bt_paired_bda,b->bt_paired_bda,6) &&
          !strcmp(a->bt_paired_name,b->bt_paired_name) &&
          !strcmp(a->u4_version,b->u4_version) &&
          !strcmp(a->song_folder,b->song_folder) &&
          !strcmp(a->song_audio,b->song_audio) &&
          a->ebook_count==b->ebook_count))
        return false;
    unsigned count=a->ebook_count;
    if (count > PLAYER_SETTINGS_EBOOK_MAX) count=PLAYER_SETTINGS_EBOOK_MAX;
    for (unsigned i=0;i<count;++i) {
        if (a->ebook[i].key!=b->ebook[i].key ||
            a->ebook[i].page!=b->ebook[i].page ||
            a->ebook[i].pages!=b->ebook[i].pages ||
            a->ebook[i].stamp!=b->ebook[i].stamp)
            return false;
    }
    return true;
}
esp_err_t player_settings_save(const player_settings_t *v)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (v->brightness<5 || v->brightness>100 || v->volume>100 ||
        !memchr(v->song_folder,0,MUSIC_NAME_MAX) || !memchr(v->song_audio,0,MUSIC_NAME_MAX) ||
        !memchr(v->bt_paired_name,0,sizeof(v->bt_paired_name)) ||
        !memchr(v->u4_version,0,sizeof(v->u4_version)) ||
        v->ebook_count > PLAYER_SETTINGS_EBOOK_MAX)
        return ESP_ERR_INVALID_ARG;
    for (unsigned i=0;i<v->ebook_count;++i) {
        const player_settings_ebook_t *e=&v->ebook[i];
        if (!e->key || !e->pages ||
            e->pages>PLAYER_SETTINGS_EBOOK_MAX_PAGES || e->page>=e->pages)
            return ESP_ERR_INVALID_ARG;
    }
    if (s_have_saved && player_settings_equal(v,&s_saved))
        return ESP_OK;
    nvs_handle_t handle;
    esp_err_t ret=nvs_open("player",NVS_READWRITE,&handle);
    if (ret!=ESP_OK) return ret;
    size_t len=encode(v,s_encoded);
    ret=nvs_set_blob(handle,"settings",s_encoded,len);
    /* 同步 U4 版本的独立小 key：U4 OTA 任务会在 PMIC 重启前单独写它，
     * 正常 3 秒快照也更新它，避免下次开机 key 比 blob 旧。缓存写入
     * 失败不阻断用户设置保存。 */
    if (ret==ESP_OK) {
        if (v->u4_version[0]) (void)nvs_set_str(handle,"u4ver",v->u4_version);
        else (void)nvs_erase_key(handle,"u4ver");
    }
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
