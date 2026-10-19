#include "audio_player.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void probe_mp3(FILE *fp,audio_file_info_t *info)
{
    uint8_t h[10];uint64_t start=0;
    if(fseeko(fp,0,SEEK_SET)!=0 || fread(h,1,10,fp)!=10)return;
    if(!memcmp(h,"ID3",3)) {
        if((h[6]|h[7]|h[8]|h[9])&0x80)return;
        start=10+((uint32_t)h[6]<<21|h[7]<<14|h[8]<<7|h[9]);
        if(h[3]==4 && (h[5]&0x10))start+=10;
    }
    if(start>=info->file_size || fseeko(fp,(off_t)start,SEEK_SET)!=0)return;
    uint32_t header=0;
    static const uint16_t rates[3]={44100,48000,32000};
    static const uint16_t bitrates[2][15]={
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},
    };
    for(uint32_t n=0;n<128*1024 && start+n<info->file_size;++n) {
        int c=fgetc(fp);if(c==EOF)return;
        header=(header<<8)|(uint8_t)c;
        if(n<3 || (header&0xFFE00000)!=0xFFE00000)continue;
        unsigned version=(header>>19)&3,layer=(header>>17)&3;
        unsigned br=(header>>12)&15,sr=(header>>10)&3;
        if(version==1 || layer!=1 || br==0 || br==15 || sr==3)continue;
        unsigned rate=rates[sr]/(version==3?1:version==2?2:4);
        unsigned kbps=bitrates[version==3][br];
        unsigned samples=version==3?1152:576;
        unsigned size=(version==3?144000:72000)*kbps/rate+((header>>9)&1);
        uint64_t offset=start+n-3;
        if(size<32 || size>2048 || offset+size>info->file_size)continue;
        uint8_t frame[2048];
        if(fseeko(fp,(off_t)offset,SEEK_SET)!=0 || fread(frame,1,size,fp)!=size)return;
        unsigned channels=((header>>6)&3)==3?1:2;
        unsigned xing=4+((header&0x10000)?0:2)+(version==3?(channels==1?17:32):(channels==1?9:17));
        uint32_t frames=0;
        if(xing+12<=size && (!memcmp(frame+xing,"Xing",4)||!memcmp(frame+xing,"Info",4)) &&
           (be32(frame+xing+4)&1))frames=be32(frame+xing+8);
        else if(size>=54 && !memcmp(frame+36,"VBRI",4))frames=be32(frame+50);
        info->sample_rate=rate;info->channels=channels;info->bits_per_sample=16;
        if(frames) {
            info->total_samples=(uint64_t)frames*samples;
            info->duration_ms=info->total_samples*1000/rate;
        } else {
            /* 无Xing/VBRI时只按真实帧头码率估算，不假设128kbps；VBR无索引
             * 可能有误差，播放结束后音频任务以实际输出样本修正时长。 */
            uint64_t end=info->file_size;
            if(end>=128 && fseeko(fp,(off_t)(end-128),SEEK_SET)==0 &&
               fread(h,1,3,fp)==3 && !memcmp(h,"TAG",3))end-=128;
            info->duration_ms=(end-offset)*8/kbps;
        }
        info->metadata_known=true;return;
    }
}

esp_err_t audio_player_probe(const char *path, audio_file_info_t *info)
{
    if (!path || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return ESP_FAIL;
    info->file_size = (uint64_t)st.st_size;
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;
    uint8_t h[42];
    size_t n = fread(h, 1, sizeof(h), fp);
    bool read_error = ferror(fp);
    if (read_error || !n) { fclose(fp); return ESP_FAIL; }
    const char *ext = strrchr(path, '.');
    info->is_flac = ext && !strcasecmp(ext, ".flac");
    if (!info->is_flac) {
        bool mp3=ext && !strcasecmp(ext,".mp3");
        if(mp3)probe_mp3(fp,info);
        fclose(fp);
        return mp3?ESP_OK:ESP_ERR_NOT_SUPPORTED;
    }
    fclose(fp);
    /* Native FLAC, RFC 9639 section 8.2: first block must be 34-byte STREAMINFO. */
    if (n != sizeof(h) || memcmp(h, "fLaC", 4) || (h[4] & 0x7f) != 0 ||
        h[5] != 0 || h[6] != 0 || h[7] != 34) return ESP_ERR_INVALID_RESPONSE;
    uint64_t packed = 0;
    for (unsigned i = 18; i < 26; ++i) packed = (packed << 8) | h[i];
    info->sample_rate = (uint32_t)(packed >> 44);
    info->channels = (uint8_t)((packed >> 41) & 7) + 1;
    info->bits_per_sample = (uint8_t)((packed >> 36) & 31) + 1;
    info->total_samples = packed & UINT64_C(0xfffffffff);
    if (!info->sample_rate || info->bits_per_sample < 4) return ESP_ERR_INVALID_RESPONSE;
    info->duration_ms = info->total_samples * UINT64_C(1000) / info->sample_rate;
    info->metadata_known = true;
    return ESP_OK;
}
