#include "mb86H/bootstrap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XZ_BODY 0x000ea61cu
#define TORNE_BODY 0x000e1000u
#define RESIDENT_BYTES 262144u

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long length;
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    length = ftell(f);
    assert(length > 0 && !fseek(f, 0, SEEK_SET));
    data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, f) == (size_t)length);
    assert(!fclose(f));
    *size = (size_t)length;
    return data;
}

static uint32_t append(uint8_t *bundle,uint32_t *cursor,const void *data,uint32_t bytes)
{ uint32_t at=(*cursor+3u)&~3u;memcpy(bundle+at,data,bytes);*cursor=at+bytes;return at; }
static void record(uint8_t *bundle,uint32_t at,uint32_t address,uint32_t bytes,uint32_t data)
{ put32(bundle+at,address);put32(bundle+at+4,bytes);put32(bundle+at+8,data);put32(bundle+at+12,0); }

/* Complete in-memory SBC2: two profiles, one small REL body carrying every
 * relocation encoding, real reference/patch tables, and a zero resident. */
static uint8_t *fixture(size_t *size)
{
    uint8_t body[80]={0},hook[88],eight[8],four[4],torne[0x1f20],*bundle;
    uint32_t cursor,body_at,reloc_at,resident_at,xz_refs,xz_patches,tn_refs,tn_patches,data;
    bundle=calloc(1,300000u);assert(bundle);
    body[0]=0x9b;put32(body+8,8);body[16]=0x9f;body[17]=0x80;put32(body+18,16);
    cursor=192;body_at=append(bundle,&cursor,body,sizeof(body));reloc_at=cursor;
    put32(bundle+cursor,0);put32(bundle+cursor+4,20);put32(bundle+cursor+8,0);
    put32(bundle+cursor+12,8);put32(bundle+cursor+16,32);put32(bundle+cursor+20,8);
    put32(bundle+cursor+24,16);put32(bundle+cursor+28,48);put32(bundle+cursor+32,16);cursor+=36;
    resident_at=cursor;cursor+=RESIDENT_BYTES;xz_refs=cursor;cursor+=64;xz_patches=cursor;cursor+=48;
    tn_refs=cursor;cursor+=16;tn_patches=cursor;cursor+=32;
    memset(hook,0x11,sizeof(hook));memset(eight,0x22,sizeof(eight));memset(four,0x33,sizeof(four));memset(torne,0x44,sizeof(torne));
    data=append(bundle,&cursor,hook,sizeof(hook));record(bundle,xz_refs,0xe9a10u,sizeof(hook),data);
    data=cursor;memcpy(bundle+data,body,sizeof(body));memset(bundle+data+sizeof(body),0x55,2236-sizeof(body));cursor+=2236;
    record(bundle,xz_refs+16,XZ_BODY,2236,data);
    data=append(bundle,&cursor,eight,sizeof(eight));record(bundle,xz_refs+32,0xe9b54u,sizeof(eight),data);
    data=append(bundle,&cursor,eight,sizeof(eight));record(bundle,xz_refs+48,0xe9c08u,sizeof(eight),data);
    data=append(bundle,&cursor,hook,sizeof(hook));record(bundle,xz_patches,0xe9a10u,sizeof(hook),data);
    data=append(bundle,&cursor,eight,sizeof(eight));record(bundle,xz_patches+16,0xe9b54u,sizeof(eight),data);
    data=append(bundle,&cursor,eight,sizeof(eight));record(bundle,xz_patches+32,0xe9c08u,sizeof(eight),data);
    data=append(bundle,&cursor,torne,sizeof(torne));record(bundle,tn_refs,0xe9000u,sizeof(torne),data);
    data=append(bundle,&cursor,eight,sizeof(eight));record(bundle,tn_patches,0xe9ac0u,sizeof(eight),data);
    data=append(bundle,&cursor,four,sizeof(four));record(bundle,tn_patches+16,0xe9d94u,sizeof(four),data);
    put32(bundle,0x53424332u);put32(bundle+4,1);put32(bundle+8,64);put32(bundle+12,cursor);put32(bundle+16,64);put32(bundle+20,2);
    put32(bundle+24,body_at);put32(bundle+28,sizeof(body));put32(bundle+32,reloc_at);put32(bundle+36,3);put32(bundle+40,resident_at);put32(bundle+44,RESIDENT_BYTES);
    put32(bundle+64,1);put32(bundle+68,XZ_BODY);put32(bundle+72,2236);put32(bundle+76,xz_refs);put32(bundle+80,4);put32(bundle+84,xz_patches);put32(bundle+88,3);put32(bundle+92,1);
    put32(bundle+128,2);put32(bundle+132,TORNE_BODY);put32(bundle+136,2048);put32(bundle+140,tn_refs);put32(bundle+144,1);put32(bundle+148,tn_patches);put32(bundle+152,2);
    *size=cursor;return bundle;
}

static void oracle_body(const uint8_t *bundle, const struct mb86H_bootstrap *b,
                        uint8_t *out)
{
    uint32_t index;
    memcpy(out, b->body, b->body_size);
    for (index = 0; index < b->relocation_count; ++index) {
        const uint8_t *r = b->relocations + index * 12u;
        uint32_t at = get32(r), type = get32(r + 4), target = get32(r + 8);
        uint32_t value = b->body_address + target;
        if (type == 20u) {
            assert(out[at] == 0x9b);
            assert((((uint32_t)(out[at + 1] >> 4) << 16) |
                    ((uint32_t)out[at + 2] << 8) | out[at + 3]) == target);
            out[at + 1] = (uint8_t)((out[at + 1] & 15u) | ((value >> 16) << 4));
            out[at + 2] = (uint8_t)(value >> 8); out[at + 3] = (uint8_t)value;
        } else if (type == 32u) {
            assert(get32(out + at) == target); put32(out + at, value);
        } else {
            assert(type == 48u && out[at] == 0x9f && (out[at + 1] >> 4) == 8u);
            assert(get32(out + at + 2) == target); put32(out + at + 2, value);
        }
    }
    (void)bundle;
}

static void check_profile(const uint8_t *bundle, size_t size, int torne)
{
    struct mb86H_bootstrap b;
    const uint8_t *resident;
    uint8_t *oracle = malloc(2236);
    uint8_t got[64];
    uint32_t index;
    size_t lengths[] = {1, 4, 64};

    assert(oracle && !mb86H_bootstrap_parse(&b, bundle, size, torne, &resident));
    assert(resident == bundle + get32(bundle + 40));
    oracle_body(bundle, &b, oracle);
    for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        size_t length = lengths[index];
        assert(!mb86H_bootstrap_read(&b, b.body_address, got, length));
        assert(!memcmp(got, oracle, length));
        assert(!mb86H_bootstrap_read(&b, b.body_address + b.body_size - length, got, length));
        assert(!memcmp(got, oracle + b.body_size - length, length));
    }
    for (index = 0; index < b.relocation_count; ++index) {
        uint32_t at = get32(b.relocations + index * 12u);
        assert(!mb86H_bootstrap_read(&b, b.body_address + at, got, 1));
        assert(got[0] == oracle[at]);
        assert(!mb86H_bootstrap_read(&b, b.body_address + at, got, 4));
        assert(!memcmp(got, oracle + at, 4));
    }
    assert(mb86H_bootstrap_read(&b, b.body_address + b.body_capacity, got, 1));
    free(oracle);
}

int main(int argc, char **argv)
{
    size_t size;
    uint8_t *bundle;
    uint8_t *bad;
    uint32_t profiles, relocs, xz_refs, xz_ref_count, xz_patches;
    uint32_t index;
    struct mb86H_bootstrap b;
    const uint8_t *resident;

    assert(argc <= 2);
    bundle = argc == 2 ? read_file(argv[1], &size) : fixture(&size);
    assert(!memcmp(bundle, "SBC2", 4));
    bad = malloc(size);
    profiles = get32(bundle + 16); relocs = get32(bundle + 32);
    if(get32(bundle+20)==1) {
        assert(get32(bundle+profiles)==3u);
        check_profile(bundle,size,MB86H_BOOTSTRAP_DT295);
        assert(mb86H_bootstrap_parse(&b,bundle,size,MB86H_BOOTSTRAP_XZ,&resident));
        assert(mb86H_bootstrap_parse(&b,bundle,size,MB86H_BOOTSTRAP_TORNE,&resident));
        memcpy(bad,bundle,size);put32(bad+20,0);
        assert(mb86H_bootstrap_parse(&b,bad,size,MB86H_BOOTSTRAP_DT295,&resident));
        free(bad);free(bundle);return 0;
    }
    xz_refs = get32(bundle + profiles + 12); xz_ref_count = get32(bundle + profiles + 16);
    xz_patches = get32(bundle + profiles + 20);
    check_profile(bundle, size, 0);
    check_profile(bundle, size, 1);
    assert(bad);
    memcpy(bad, bundle, size); put32(bad + 12, (uint32_t)size - 1u);
    assert(mb86H_bootstrap_parse(&b, bad, size, 0, &resident));
    memcpy(bad, bundle, size); put32(bad + relocs + 4, 99u);
    assert(mb86H_bootstrap_parse(&b, bad, size, 0, &resident));
    memcpy(bad,bundle,size);put32(bad+profiles,3u);
    assert(mb86H_bootstrap_parse(&b,bad,size,0,&resident));
    memcpy(bad,bundle,size);put32(bad+36,0xffffffffu);
    assert(mb86H_bootstrap_parse(&b,bad,size,0,&resident));
    memcpy(bad,bundle,size);put32(bad+profiles+12,get32(bundle+24));
    assert(mb86H_bootstrap_parse(&b,bad,size,0,&resident));
    memcpy(bad, bundle, size); put32(bad + xz_patches, XZ_BODY);
    assert(mb86H_bootstrap_parse(&b, bad, size, 0, &resident));
    memcpy(bad, bundle, size);
    for (index = 0; index < xz_ref_count; ++index) {
        if (get32(bad + xz_refs + index * 16u) == XZ_BODY) {
            put32(bad + xz_refs + index * 16u + 4, 1u);
            break;
        }
    }
    assert(index != xz_ref_count);
    assert(mb86H_bootstrap_parse(&b, bad, size, 0, &resident));
    free(bad); free(bundle);
    return 0;
}
