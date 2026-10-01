#include "random.h"
#include "spinlock.h"
#include "string.h"

// A ChaCha20-based generator. The key is seeded from RDSEED/RDRAND when the
// CPU has them and from timestamp jitter, and is stirred with interrupt timing
// (random_mix) for the life of the system. Each output block re-keys the
// generator, so earlier output cannot be recovered from a later state.

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), \
                        a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))

static spinlock_t lock = SPINLOCK_INIT;
static uint32_t key[8];
static uint64_t counter;
static uint32_t pool[16];
static uint32_t pool_pos;

void chacha20_block(const uint32_t k[8], uint64_t ctr, const uint32_t nonce[2], uint32_t out[16])
{
    uint32_t s[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
        (uint32_t)ctr, (uint32_t)(ctr >> 32), nonce ? nonce[0] : 0, nonce ? nonce[1] : 0,
    };
    uint32_t x[16];

    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + s[i];
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static bool hw_random(uint64_t *out, bool seed)
{
    for (int tries = 0; tries < 32; tries++) {
        uint8_t ok;

        if (seed)
            __asm__ volatile ("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok));
        else
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok)
            return true;
    }
    return false;
}

// Folds the pool into the key. Called with the lock held.
static void stir(void)
{
    uint32_t out[16];

    for (int i = 0; i < 8; i++)
        key[i] ^= pool[i] ^ ROTL(pool[i + 8], 13);
    chacha20_block(key, counter++, NULL, out);
    memcpy(key, out, sizeof(key));
    memset(pool, 0, sizeof(pool));
    memset(out, 0, sizeof(out));
}

void random_init(void)
{
    uint32_t r[4];
    bool has_rdrand, has_rdseed;
    uint64_t flags;

    cpuid(1, 0, r);
    has_rdrand = r[2] & (1U << 30);
    cpuid(0, 0, r);
    has_rdseed = false;
    if (r[0] >= 7) {
        cpuid(7, 0, r);
        has_rdseed = r[1] & (1U << 18);
    }

    flags = spin_lock_irqsave(&lock);
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;

        if (!(has_rdseed && hw_random(&v, true)) && has_rdrand)
            hw_random(&v, false);
        // Timestamp jitter around a short busy loop adds entropy either way.
        for (int j = 0; j < 64; j++) {
            uint64_t t = rdtsc();

            for (volatile int k = 0; k < (int)(t & 63); k++)
                ;
            v ^= rdtsc() << (j % 32);
        }
        pool[i] ^= (uint32_t)v ^ (uint32_t)(v >> 32);
    }
    stir();
    spin_unlock_irqrestore(&lock, flags);
}

// Adds an event's timing to the pool; cheap enough for interrupt handlers.
void random_mix(uint64_t data)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint64_t t = rdtsc() ^ data;

    pool[pool_pos % 16] ^= (uint32_t)t ^ ROTL((uint32_t)(t >> 32), pool_pos % 31 + 1);
    if (++pool_pos % 64 == 0)
        stir();
    spin_unlock_irqrestore(&lock, flags);
}

void random_bytes(void *buf, size_t n)
{
    uint8_t *p = buf;
    uint32_t block[16];

    while (n) {
        uint64_t flags = spin_lock_irqsave(&lock);
        size_t chunk;

        chacha20_block(key, counter++, NULL, block);
        // The first 32 bytes become the next key; the rest are output.
        memcpy(key, block, sizeof(key));
        spin_unlock_irqrestore(&lock, flags);

        chunk = MIN(n, (size_t)32);
        memcpy(p, (uint8_t *)block + 32, chunk);
        p += chunk;
        n -= chunk;
    }
    memset(block, 0, sizeof(block));
}

uint64_t random_u64(void)
{
    uint64_t v;

    random_bytes(&v, sizeof(v));
    return v;
}
