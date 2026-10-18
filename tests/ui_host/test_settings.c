/* Real serializer/NVS integration with a reopenable, commit-aware NVS double.
 * This does not replace physical flash persistence/cut-power bench tests. */
#include <assert.h>
#include <stdio.h>
#include "../../main/player_settings.c"
static ui_device_state_t state;
static uint8_t disk[SETTINGS_MAX], pending[SETTINGS_MAX];
static size_t disk_len, pending_len;
static unsigned opens, commits, sets;
static bool opened, writable, fail_commit, corrupt_read;
static esp_err_t init_result;
ui_device_state_t *ui_get_device_state(void) { return &state; }
esp_err_t nvs_flash_init(void) { return init_result; }
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h)
{ assert(!opened && !strcmp(ns,"player")); opened=true; writable=mode==NVS_READWRITE; *h=1; ++opens; return ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h==1 && opened); opened=false; pending_len=0; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *len)
{
    assert(h==1 && opened && !strcmp(key,"settings"));
    if (!disk_len) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len=disk_len; return ESP_OK; }
    if (*len<disk_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out,disk,disk_len); *len=disk_len;
    if (corrupt_read) ((uint8_t *)out)[12]^=1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t len)
{ assert(opened && writable && h==1 && !strcmp(key,"settings") && len<=SETTINGS_MAX); memcpy(pending,data,len); pending_len=len; ++sets; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h==1 && opened && writable); ++commits;
    if (fail_commit) return ESP_FAIL;
    memcpy(disk,pending,pending_len); disk_len=pending_len; return ESP_OK;
}
static void reboot_ram(void)
{
    s_ready=s_have_saved=false;
    s_ram=(player_settings_t){.brightness=80,.volume=50};
    s_saved=s_ram;
    memset(&state,0,sizeof(state));
}
int main(void)
{
    assert(player_settings_init()==ESP_OK && s_ready && !s_have_saved);
    player_settings_restore_ui(); assert(state.brightness==80 && state.volume==50);
    state.manual_brightness=29; state.brightness=29; state.volume=0; state.muted=true; state.auto_brightness=true;
    music_item_t item; memset(&item,0,sizeof(item)); strcpy(item.name,"中文歌曲目录"); strcpy(item.audio,"同名音频.flac");
    player_settings_set_song(&item);
    player_settings_t before,after;
    player_settings_capture(&before);
    assert(player_settings_save(&before)==ESP_OK && commits==1 && opens>=3);
    assert(player_settings_save(&before)==ESP_OK && commits==1); /* unchanged: no flash write */
    reboot_ram(); assert(player_settings_init()==ESP_OK);
    player_settings_restore_ui(); player_settings_capture(&after);
    assert(player_settings_equal(&before,&after));
    assert(state.brightness==29 && state.volume==0 && state.muted && state.auto_brightness);
    state.brightness=95; /* Automatically changed brightness is not a saved preference. */
    player_settings_capture(&after);
    assert(after.brightness==29 && player_settings_equal(&before,&after));
    assert(player_settings_save(&after)==ESP_OK && commits==1);
    assert(state.manual_brightness==29);
    puts("PASS automatic brightness changes do not write NVS or replace manual preference");
    puts("PASS versioned UTF-8 song/settings roundtrip, commit/reopen/readback and unchanged write suppression");
    after.volume=66; fail_commit=true;
    assert(player_settings_save(&after)==ESP_FAIL && s_saved.volume==0);
    fail_commit=false; assert(player_settings_save(&after)==ESP_OK);
    after.volume=67; corrupt_read=true;
    assert(player_settings_save(&after)!=ESP_OK && s_saved.volume==66);
    corrupt_read=false; assert(player_settings_save(&after)==ESP_OK);
    puts("PASS failed commit/readback never marked saved; subsequent retry succeeds");
    uint8_t encoded[SETTINGS_MAX]; size_t len=encode(&before,encoded);
    for (size_t n=0;n<len;++n) assert(decode(encoded,n,&after)!=ESP_OK);
    encoded[len-1]^=1; assert(decode(encoded,len,&after)==ESP_ERR_INVALID_RESPONSE);
    encoded[len-1]^=1;
    size_t extra=add_field(encoded,len,90,"xyz",3);
    put16(encoded+6,extra); put32(encoded+8,crc32(encoded+12,extra-12));
    assert(decode(encoded,extra,&after)==ESP_OK && player_settings_equal(&before,&after));
    len=encode(&before,encoded); encoded[15]=101; /* field 1: brightness */
    put32(encoded+8,crc32(encoded+12,len-12)); assert(decode(encoded,len,&after)==ESP_ERR_INVALID_ARG);
    len=encode(&before,encoded); extra=add_field(encoded,len,1,"x",1);
    put16(encoded+6,extra); put32(encoded+8,crc32(encoded+12,extra-12));
    assert(decode(encoded,extra,&after)==ESP_ERR_INVALID_RESPONSE);
    puts("PASS truncation/CRC/range/duplicate rejection and unknown TLV extension handling");
    disk_len=encode(&before,disk); put16(disk+4,2); reboot_ram();
    assert(player_settings_init()==ESP_ERR_NOT_SUPPORTED && !s_ready);
    unsigned written=sets;
    assert(player_settings_save(&before)==ESP_ERR_INVALID_STATE && sets==written && get16(disk+4)==2);
    reboot_ram(); init_result=ESP_FAIL;
    assert(player_settings_init()==ESP_FAIL && !s_ready && sets==written);
    puts("PASS unsupported versions/init failures preserve NVS and disable writes; no erase API exists in double");
    return 0;
}
