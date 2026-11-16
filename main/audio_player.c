#include "audio_player.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>


static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint16_t le16(const uint8_t *p)
{return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static bool read_exact(FILE *fp,void *buf,size_t n)
{return fread(buf,1,n,fp)==n;}

/* RIFF/WAVE PCM header probe. Keep the same format scope as video_wav.c:
 * PCM (format 1), 16/24/32-bit, mono/stereo. The data chunk is clamped to
 * the real file end and floored to whole frames; duration is exact. */
static esp_err_t probe_wav(FILE *fp,audio_file_info_t *info)
{
    if(fseeko(fp,0,SEEK_SET)!=0)return ESP_FAIL;
    uint8_t hdr[12];
    if(!read_exact(fp,hdr,sizeof(hdr)) || memcmp(hdr,"RIFF",4) ||
       memcmp(hdr+8,"WAVE",4))return ESP_ERR_INVALID_RESPONSE;

    bool have_fmt=false,have_data=false;
    uint16_t format=0,channels=0,block_align=0,bits=0;
    uint32_t rate=0;
    uint64_t data_offset=0,data_bytes=0;
    for(unsigned chunk=0;chunk<64;++chunk) {
        uint8_t ch[8];
        if(!read_exact(fp,ch,sizeof(ch)))break;
        uint32_t size=le32(ch+4);
        off_t body=ftello(fp);
        if(body<0)return ESP_ERR_INVALID_RESPONSE;
        if(!memcmp(ch,"fmt ",4)) {
            uint8_t fmt[40];
            size_t take=size<sizeof(fmt)?size:sizeof(fmt);
            if(size<16 || !read_exact(fp,fmt,take))return ESP_ERR_INVALID_RESPONSE;
            format=le16(fmt);
            channels=le16(fmt+2);
            rate=le32(fmt+4);
            block_align=le16(fmt+12);
            bits=le16(fmt+14);
            /* 24-bit WAV 常用 WAVE_FORMAT_EXTENSIBLE：32-byte fmt 扩展，
             * SubFormat GUID 才是真正的编码。只接受 PCM 子格式。 */
            if(format==0xFFFE) {
                static const uint8_t pcm_guid[16]={
                    0x01,0x00,0x00,0x00,0x00,0x00,0x10,0x00,
                    0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71};
                uint16_t cb=size>=18 ? le16(fmt+16) : 0;
                uint16_t valid=size>=20 ? le16(fmt+18) : 0;
                if(size<40 || take<40 || cb<22 || !valid || valid>bits ||
                   memcmp(fmt+24,pcm_guid,sizeof(pcm_guid))) {
                    ESP_LOGW("audio_player","WAV extensible unsupported: cb=%u valid=%u bits=%u",
                             cb,valid,bits);
                    return ESP_ERR_NOT_SUPPORTED;
                }
                format=1;
            }
            have_fmt=true;
            uint64_t skip=(uint64_t)size-take+(size&1U);
            if(skip && fseeko(fp,(off_t)skip,SEEK_CUR)!=0)return ESP_FAIL;
        } else if(!memcmp(ch,"data",4)) {
            data_offset=(uint64_t)body;
            data_bytes=size;
            have_data=true;
            break;
        } else {
            /* JUNK/LIST/fact 等：按 chunk 长度和 1 字节 padding 跳过。 */
            if(size>0x7ffffff0U ||
               fseeko(fp,(off_t)(size+(size&1U)),SEEK_CUR)!=0)
                return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if(!have_fmt || !have_data || data_offset>info->file_size)
        return ESP_ERR_INVALID_RESPONSE;
    if(format!=1 || (channels!=1 && channels!=2) ||
       (bits!=16 && bits!=24 && bits!=32) || rate==0 ||
       block_align!=(uint16_t)(channels*(bits/8))) {
        ESP_LOGW("audio_player","WAV unsupported: tag=0x%04x ch=%u rate=%lu bits=%u align=%u",
                 format,channels,(unsigned long)rate,bits,block_align);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t available=info->file_size-data_offset;
    if(data_bytes==0 || data_bytes==UINT32_MAX || data_bytes>available)
        data_bytes=available;
    uint64_t frames=data_bytes/block_align;
    if(!frames)return ESP_ERR_NOT_FOUND;
    data_bytes=frames*block_align;

    info->sample_rate=rate;
    info->channels=(uint8_t)channels;
    info->bits_per_sample=(uint8_t)bits;
    info->total_samples=frames;
    info->duration_ms=frames*1000/rate;
    info->data_offset=data_offset;
    info->data_bytes=data_bytes;
    info->is_wav=true;
    info->metadata_known=true;
    return ESP_OK;
}
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
    if (ext && !strcasecmp(ext, ".wav")) {
        esp_err_t ret = probe_wav(fp, info);
        fclose(fp);
        return ret;
    }
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
