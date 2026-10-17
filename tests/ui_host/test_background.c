/* Actual preprocessing/cache with guarded host allocations; no JPEG or LVGL
 * mocks in the image pipeline. Single-threaded ownership, not an SMP stress test. */
#include <assert.h>
#include <stdio.h>
#include "../../main/music_background.c"

void test_fail_after(int calls);
size_t test_live_bytes(void);
static uint16_t cover[COVER_WIDTH * COVER_HEIGHT];

static void fill(uint16_t color)
{
    for (unsigned i=0;i<COVER_WIDTH*COVER_HEIGHT;i++) cover[i]=color;
}
static uint16_t tint(unsigned r, unsigned g, unsigned b)
{
    r=(r*(255-MUSIC_BACKGROUND_WHITE_OPA)+255*MUSIC_BACKGROUND_WHITE_OPA+127)/255;
    g=(g*(255-MUSIC_BACKGROUND_WHITE_OPA)+255*MUSIC_BACKGROUND_WHITE_OPA+127)/255;
    b=(b*(255-MUSIC_BACKGROUND_WHITE_OPA)+255*MUSIC_BACKGROUND_WHITE_OPA+127)/255;
    return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}
static void uniform(const uint8_t *pixels, uint16_t expected)
{
    const uint16_t *image=(const uint16_t *)pixels;
    for (unsigned i=0;i<MUSIC_BACKGROUND_WIDTH*MUSIC_BACKGROUND_HEIGHT;i++) assert(image[i]==expected);
}
int main(void)
{
    for (int fail=0;fail<4;fail++) {
        test_fail_after(fail);
        assert(music_background_init()==ESP_ERR_NO_MEM && test_live_bytes()==0);
    }
    test_fail_after(-1);
    assert(music_background_init()==ESP_OK);
    size_t fixed_bytes=MUSIC_BACKGROUND_SLOTS*OUTPUT_BYTES+2*SMALL_BYTES;
    assert(test_live_bytes()==fixed_bytes);
    /* No per-song allocations, including the transform's work images. */
    test_fail_after(0);
    fill(0xf800);
    const uint8_t *red=NULL, *image=NULL;
    assert(music_background_generate((const uint8_t *)cover,1,0,&red)==ESP_OK);
    uniform(red,tint(255,0,0));
    for (unsigned i=0;i<COVER_WIDTH*COVER_HEIGHT;i++) assert(cover[i]==0xf800);
    fill(0x001f);
    assert(music_background_generate((const uint8_t *)cover,1,0,&image)==ESP_OK && image==red);
    music_background_release(image);
    uniform(red,tint(255,0,0)); /* Cache hit ignores replacement input, preserving original pixels. */

    for (unsigned y=0;y<COVER_HEIGHT;y++)
        for (unsigned x=0;x<COVER_WIDTH;x++) cover[y*COVER_WIDTH+x]=((x+y)&1)?0xffff:0;
    assert(music_background_generate((const uint8_t *)cover,1,1,&image)==ESP_OK);
    unsigned middle=((const uint16_t *)image)[230*460+230];
    assert(((middle>>11)&31)>=17 && ((middle>>11)&31)<=19);
    assert((middle&31)>=17 && (middle&31)<=19); /* High-frequency checker becomes smooth gray. */
    music_background_release(image);

    for (unsigned y=0;y<COVER_HEIGHT;y++)
        for (unsigned x=0;x<COVER_WIDTH;x++) cover[y*COVER_WIDTH+x]=x<COVER_WIDTH/2?0xf800:0x001f;
    const uint8_t *ramp=NULL;
    assert(music_background_generate((const uint8_t *)cover,1,2,&ramp)==ESP_OK);
    const uint16_t *row=(const uint16_t *)ramp+230*460;
    assert(row[0]==tint(255,0,0) && row[459]==tint(0,0,255));
    assert(((row[230]>>11)&31)>8 && (row[230]&31)>8); /* Blended across the original sharp edge. */
    for (unsigned x=1;x<460;x++) {
        assert((row[x]>>11)<=(row[x-1]>>11));
        assert((row[x]&31)>=(row[x-1]&31));
        assert(((const uint16_t *)ramp)[x]==row[x]); /* Clamped top edge has no dark border. */
    }
    uint16_t held_middle=row[230];
    for (unsigned i=0;i<100;i++) {
        fill((i&1)?0xffff:0x07e0);
        assert(music_background_generate((const uint8_t *)cover,1,10+i,&image)==ESP_OK);
        uniform(image,(i&1)?tint(255,255,255):tint(0,255,0));
        music_background_release(image);
        assert(test_live_bytes()==fixed_bytes);
        assert(((const uint16_t *)red)[0]==tint(255,0,0) && row[230]==held_middle);
    }
    music_background_invalidate();
    assert(!music_background_acquire(1,0));
    assert(((const uint16_t *)red)[0]==tint(255,0,0));
    fill(0);
    assert(music_background_generate((const uint8_t *)cover,2,0,&image)==ESP_OK && image!=red);
    uniform(image,tint(0,0,0));
    const uint8_t *busy=(void *)1;
    assert(music_background_generate((const uint8_t *)cover,2,1,&busy)==ESP_ERR_INVALID_STATE && !busy);
    music_background_release(image);
    music_background_release(red);
    music_background_release(ramp);
    music_background_destroy();
    assert(test_live_bytes()==0);
    puts("PASS RGB565 colors/white mix, checker blur, bilinear edges, untouched covers, bounded pinned cache and cleanup");
    puts("ALL BACKGROUND TESTS PASSED");
    return 0;
}
