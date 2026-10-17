#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

typedef union { struct { size_t size; uint64_t magic; } h; long double align; } allocation_t;
static size_t live_bytes, peak_bytes;
static int fail_after = -1;
static bool inserted = true;
bool sd_card_is_present(void) { return inserted; }
void test_set_inserted(bool present) { inserted = present; }
void test_fail_after(int calls) { fail_after = calls; }
size_t test_live_bytes(void) { return live_bytes; }
size_t test_peak_bytes(void) { return peak_bytes; }
void *test_heap_malloc(size_t size, unsigned caps)
{
    (void)caps;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    allocation_t *h = malloc(sizeof(*h) + size + 16);
    if (!h) return NULL;
    h->h.size = size; h->h.magic = UINT64_C(0x1122334455667788);
    unsigned char *data = (unsigned char *)(h + 1);
    memset(data + size, 0xa5, 16);
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return data;
}
void *test_heap_calloc(size_t count, size_t size, unsigned caps)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *ptr = test_heap_malloc(count * size, caps);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
void test_heap_free(void *ptr)
{
    if (!ptr) return;
    allocation_t *h = (allocation_t *)ptr - 1;
    assert(h->h.magic == UINT64_C(0x1122334455667788));
    for (unsigned i = 0; i < 16; ++i) assert(((unsigned char *)ptr)[h->h.size + i] == 0xa5);
    live_bytes -= h->h.size;
    h->h.magic = 0;
    free(h);
}
