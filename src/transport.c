#include "transport.h"
#include <string.h>
static const uint8_t magic[16] = {0x61, 0xdd, 0x3c, 0x35, 0xee, 0xd8, 0x46, 0xb9,
                                  0x87, 0x12, 0xd6, 0xdd, 0x21, 0x08, 0xbe, 0xb3};
static unsigned be16(const uint8_t *p) {
    return p[0] * 256u + p[1];
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint32_t ts_crc(const uint8_t *p, size_t n) {
    uint32_t crc = 0xffffffff;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
    }
    return crc;
}
void ts_check_init(ts_check *c) {
    memset(c, 0, sizeof(*c));
    c->psi_count = 1;
}
static void section_done(ts_check *c, unsigned index, size_t n) {
    uint8_t *p = c->psi[index].data;
    if (n < 8 || ts_crc(p, n)) {
        c->crc_errors++;
        return;
    }
    if (index == 0 && p[0] == 0) {
        c->pat++;
        for (size_t i = 8; i + 4 <= n - 4; i += 4)
            if (be16(p + i)) {
                unsigned pid = be16(p + i + 2) & 8191, j;
                for (j = 1; j < c->psi_count; j++)
                    if (c->psi[j].pid == pid)
                        break;
                if (j == c->psi_count && j < 65)
                    c->psi[c->psi_count++].pid = (uint16_t)pid;
            }
    } else if (p[0] == 2)
        c->pmt++;
}
static void section_data(ts_check *c, unsigned index, const uint8_t *p, size_t n, int allow_new) {
    while (n) {
        size_t used = c->psi[index].used;
        if (!used && (!allow_new || *p == 255))
            return;
        size_t want = used < 3 ? 3 : 3 + (be16(c->psi[index].data + 1) & 4095);
        if (want > 4096 || want < 3) {
            c->invalid++;
            c->psi[index].used = 0;
            return;
        }
        size_t take = want - used;
        if (take > n)
            take = n;
        memcpy(c->psi[index].data + used, p, take);
        p += take;
        n -= take;
        used += take;
        c->psi[index].used = used;
        if (used >= 3 && used == 3 + (size_t)(be16(c->psi[index].data + 1) & 4095)) {
            section_done(c, index, used);
            c->psi[index].used = 0;
            if (!allow_new)
                return;
        }
    }
}
void ts_check_packet(ts_check *c, const uint8_t p[188]) {
    c->packets++;
    if (p[0] != 0x47) {
        c->invalid++;
        return;
    }
    unsigned pid = be16(p + 1) & 8191, af = (p[3] >> 4) & 3, cc = p[3] & 15;
    if (p[1] & 128)
        c->tei++;
    if (p[3] & 192)
        c->scrambled++;
    if (!af || (af & 2 && p[4] > 183)) {
        c->invalid++;
        return;
    }
    int discontinuity = (af & 2) && p[4] && (p[5] & 128);
    if (discontinuity)
        c->seen[pid] = 0;
    if (!(af & 1))
        return;
    int gap = 0;
    if (pid != 8191 && c->seen[pid]) {
        if (cc == c->cc[pid] && !memcmp(c->last[pid], p, 188))
            return;
        if (cc != ((c->cc[pid] + 1) & 15)) {
            c->continuity++;
            gap = 1;
        }
    }
    c->seen[pid] = 1;
    c->cc[pid] = (uint8_t)cc;
    memcpy(c->last[pid], p, 188);
    unsigned index;
    for (index = 0; index < c->psi_count; index++)
        if (c->psi[index].pid == pid)
            break;
    if (index == c->psi_count)
        return;
    if (gap || discontinuity || p[1] & 128)
        c->psi[index].used = 0;
    size_t start = af & 2 ? 5 + p[4] : 4;
    if (start >= 188)
        return;
    if (p[1] & 64) {
        size_t ptr = p[start++];
        if (ptr > 188 - start) {
            c->invalid++;
            c->psi[index].used = 0;
            return;
        }
        if (c->psi[index].used)
            section_data(c, index, p + start, ptr, 0);
        c->psi[index].used = 0;
        start += ptr;
        section_data(c, index, p + start, 188 - start, 1);
    } else if (c->psi[index].used)
        section_data(c, index, p + start, 188 - start, 0);
}
void ts_decoder_init(ts_decoder *d, const uint8_t context[32], ts_sink sink, void *user,
                     ts_check *check) {
    memset(d, 0, sizeof(*d));
    aes128 a;
    aes_init(&a, context + 16);
    aes_encrypt(&a, context, d->encrypted_kk);
    secure_clear(&a, sizeof(a));
    d->sink = sink;
    d->user = user;
    d->check = check;
}
static int decode_fail(ts_decoder *d, const char *message) {
    d->error = message;
    return -1;
}
static int marker(ts_decoder *d, const uint8_t *p) {
    d->markers++;
    if ((p[7] & 0xf0) == 0xf0) {
        unsigned index = be16(p + 26), last = be16(p + 28);
        if (last >= FRAGMENT_LIMIT || index > last)
            return decode_fail(d, "Invalid key table fragment bounds");
        if (index == 0) {
            d->fragment = last;
            d->next_fragment = 0;
        }
        if (index != d->next_fragment || last != d->fragment)
            return decode_fail(d, "Unexpected key table fragment order");
        memcpy(d->table + index * 158, p + 30, 158);
        d->next_fragment++;
        if (index == last) {
            size_t bytes = (last + 1) * 158;
            uint32_t count = be32(d->table);
            if (count > KEY_LIMIT || 36 + (size_t)count * 32 > bytes)
                return decode_fail(d, "Invalid key table length");
            uint8_t kk[16];
            aes128 a;
            aes_init(&a, d->table + 4);
            aes_decrypt(&a, d->encrypted_kk, kk);
            aes_ctr c;
            ctr_init(&c, kk, d->table + 20, 16);
            ctr_xor(&c, d->table + 36, count * 32);
            /* The device publishes the next table before retiring the old IDs. */
            secure_clear(d->previous_key_ids, sizeof(d->previous_key_ids));
            secure_clear(d->previous_keys, sizeof(d->previous_keys));
            memcpy(d->previous_key_ids, d->key_ids, d->key_count * 16);
            memcpy(d->previous_keys, d->keys, d->key_count * 16);
            d->previous_key_count = d->key_count;
            secure_clear(d->key_ids, sizeof(d->key_ids));
            secure_clear(d->keys, sizeof(d->keys));
            for (unsigned i = 0; i < count; i++) {
                memcpy(d->key_ids[i], d->table + 36 + i * 32, 16);
                memcpy(d->keys[i], d->table + 52 + i * 32, 16);
            }
            d->key_count = count;
            d->key_tables++;
            secure_clear(kk, 16);
            secure_clear(&a, sizeof(a));
            secure_clear(&c, sizeof(c));
            secure_clear(d->table, sizeof(d->table));
        }
    } else {
        unsigned i;
        const uint8_t *key = NULL;
        for (i = 0; i < d->key_count; i++) {
            if (!memcmp(d->key_ids[i], p + 40, 16)) {
                key = d->keys[i];
                break;
            }
        }
        if (!key) {
            for (i = 0; i < d->previous_key_count; i++) {
                if (!memcmp(d->previous_key_ids[i], p + 40, 16)) {
                    key = d->previous_keys[i];
                    d->previous_key_uses++;
                    break;
                }
            }
        }
        if (!key) {
            d->active = 0;
            return decode_fail(d, "Stream key ID is absent from current and previous tables");
        }
        ctr_init(&d->ctr, key, p + 56, 16);
        d->active = 1;
        d->iv_updates++;
    }
    return 0;
}
static int feed_part(ts_decoder *d, const uint8_t *p, size_t n) {
    memcpy(d->buffer + d->used, p, n);
    d->used += n;
    size_t off = 0, outn = 0;
    uint8_t out[32768 + 564];
    while (d->used - off >= 188) {
        uint8_t *packet = d->buffer + off;
        if (packet[0] != 0x47) {
            size_t i = 1;
            while (i + 376 < d->used - off) {
                if (packet[i] == 0x47 && packet[i + 188] == 0x47 && packet[i + 376] == 0x47)
                    break;
                i++;
            }
            if (i + 376 >= d->used - off)
                i = d->used - off > 376 ? d->used - off - 376 : 0;
            d->resync += i;
            d->active = 0;
            off += i;
            if (d->used - off < 564)
                break;
            continue;
        }
        d->input++;
        off += 188;
        if (!memcmp(packet, "\x47\x00\x00\x20\xb7\x02\xb5", 7) && !memcmp(packet + 8, magic, 16)) {
            if (marker(d, packet))
                return -1;
            continue;
        }
        unsigned sc = packet[3] >> 6;
        if (sc == 1) {
            if (!d->active) {
                d->waiting++;
                continue;
            }
            ctr_xor(&d->ctr, packet + 4, 184);
            packet[3] &= 0x3f;
        }
        if (d->check)
            ts_check_packet(d->check, packet);
        memcpy(out + outn, packet, 188);
        outn += 188;
        d->output++;
    }
    if (outn && d->sink(d->user, out, outn))
        return -1;
    memmove(d->buffer, d->buffer + off, d->used - off);
    d->used -= off;
    return 0;
}
int ts_decoder_feed(ts_decoder *d, const uint8_t *p, size_t n) {
    if (d->failed)
        return -1;
    while (n) {
        size_t take = n < 32768 ? n : 32768;
        if (feed_part(d, p, take)) {
            d->failed = 1;
            return -1;
        }
        p += take;
        n -= take;
    }
    return 0;
}
