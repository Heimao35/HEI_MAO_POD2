/* Exercise the EXACT patched IDF destructor extracted at configure time.
 * Old code decrements the existing cover decoder's reference on a partial
 * second decoder: this mock deliberately refuses an unowned/null release. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_RETURN_ON_FALSE(ok,err,tag,msg) do{if(!(ok))return(err);}while(0)
#define ESP_RETURN_ON_ERROR(call,tag,msg) do{int r=(call);if(r)return r;}while(0)
typedef struct {
    void *rxlink,*txlink,*header_info,*trans_desc,*evt_queue,*dma2d_group_handle,*intr_handle;
    void *codec_base;
} decoder_t;
typedef decoder_t *jpeg_decoder_handle_t;
static unsigned refs=1,releases,freed,queues,pools,interrupts;
static const void *cover_codec=(void*)7;
static void counted_free(void *p){if(p){++freed;free(p);}}
static void vQueueDeleteWithCaps(void *p){assert(p==(void*)1);++queues;}
static void dma2d_release_pool(void *p){assert(p==(void*)2);++pools;}
static void jpeg_isr_deregister(void *codec,void *isr)
{assert(codec==cover_codec && isr==(void*)3);++interrupts;}
static esp_err_t jpeg_release_codec_handle(void *codec)
{assert(codec==cover_codec && refs>0);--refs;++releases;return ESP_OK;}
#define free counted_free
#include JPEG_DELETE_UNDER_TEST
#undef free
int main(void)
{
    assert(jpeg_del_decoder_engine(NULL)==ESP_ERR_INVALID_ARG);
    /* Existing cover decoder stays alive through 3 failures before acquisition:
     * rxlink failure, txlink failure, header_info failure. */
    for(unsigned stage=0;stage<3;++stage) {
        decoder_t *d=calloc(1,sizeof(*d));assert(d);
        if(stage>0)d->rxlink=malloc(128);
        if(stage>1)d->txlink=malloc(128);
        unsigned before=freed;
        assert(jpeg_del_decoder_engine(d)==ESP_OK);
        assert(freed==before+stage+1 && refs==1 && !releases);
    }
    /* A failure AFTER acquisition must release exactly its own reference. */
    decoder_t *d=calloc(1,sizeof(*d));d->codec_base=(void*)cover_codec;++refs;
    d->rxlink=malloc(128);d->txlink=malloc(128);d->header_info=malloc(64);
    d->trans_desc=malloc(64);d->evt_queue=(void*)1;d->dma2d_group_handle=(void*)2;d->intr_handle=(void*)3;
    unsigned before=freed;
    assert(jpeg_del_decoder_engine(d)==ESP_OK && refs==1 && releases==1);
    assert(freed==before+5 && queues==1 && pools==1 && interrupts==1);
    /* Simulate a later successful second decoder and normal deletion. */
    d=calloc(1,sizeof(*d));d->codec_base=(void*)cover_codec;++refs;
    assert(jpeg_del_decoder_engine(d)==ESP_OK && refs==1 && releases==2);
    puts("PASS actual IDF JPEG partial cleanup never releases unacquired codec; existing cover owner survives");
    return 0;
}
