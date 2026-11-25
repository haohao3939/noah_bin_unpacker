#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

typedef struct {
    uint32_t first_code;
    uint16_t sym_offset;
    uint16_t count;
} HuffLevel;

typedef struct {
    HuffLevel levels[32];
    int max_len;
    uint8_t *table_buf;
    uint16_t table_len;
} HuffTable;

typedef struct {
    uint32_t rel_off;
    uint32_t index;
} JpgEntry;

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u24(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static int cmp_jpg(const void *a, const void *b) {
    const JpgEntry *ja = (const JpgEntry *)a;
    const JpgEntry *jb = (const JpgEntry *)b;
    if (ja->rel_off < jb->rel_off) return -1;
    if (ja->rel_off > jb->rel_off) return 1;
    return 0;
}

static int find_jpg_index(const JpgEntry *map, uint32_t count, uint32_t rel_off) {
    JpgEntry key = { rel_off, 0 };
    const JpgEntry *res = (const JpgEntry *)bsearch(&key, map, count, sizeof(JpgEntry), cmp_jpg);
    return res ? (int)res->index : -1;
}

static int parse_huff_table(HuffTable *ht, const uint8_t *buf, uint16_t len) {
    ht->table_buf = (uint8_t *)malloc(len);
    if (!ht->table_buf) return -1;
    memcpy(ht->table_buf, buf, len);
    ht->table_len = len;

    uint16_t first_sym_off = 0;
    for (uint16_t off = 4; off + 2 <= len; off += 6) {
        uint16_t sym_off = read_u16(buf + off);
        if (sym_off != 0) {
            first_sym_off = sym_off;
            break;
        }
    }
    if (first_sym_off == 0 || (first_sym_off % 6) != 0) {
        free(ht->table_buf);
        ht->table_buf = NULL;
        return -1;
    }

    int max_len = first_sym_off / 6;
    if (max_len > 32) max_len = 32;
    ht->max_len = max_len;

    for (int i = 0; i < max_len; i++) {
        uint32_t fc = read_u32(buf + i * 6);
        uint16_t off = read_u16(buf + i * 6 + 4);
        ht->levels[i].first_code = fc;
        ht->levels[i].sym_offset = off;
        if (off == 0) {
            ht->levels[i].count = 0;
        } else {
            uint16_t next_off = len;
            for (int j = i + 1; j < max_len; j++) {
                uint16_t noff = read_u16(buf + j * 6 + 4);
                if (noff != 0) {
                    next_off = noff;
                    break;
                }
            }
            ht->levels[i].count = (next_off >= off) ? (uint16_t)(next_off - off) : 0;
        }
    }
    return 0;
}

static int read_bit(const uint8_t *data, size_t data_len, size_t *bit_pos) {
    if (*bit_pos >= data_len * 8) return -1;
    size_t byte_idx = *bit_pos / 8;
    int bit_idx = 7 - (int)(*bit_pos % 8);
    int bit = (data[byte_idx] >> bit_idx) & 1;
    (*bit_pos)++;
    return bit;
}

static uint32_t read_u24_bits(const uint8_t *data, size_t data_len, size_t *bit_pos) {
    uint32_t val = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t byte_val = 0;
        for (int b = 0; b < 8; b++) {
            int bit = read_bit(data, data_len, bit_pos);
            if (bit < 0) return val;
            byte_val = (uint8_t)((byte_val << 1) | bit);
        }
        val |= ((uint32_t)byte_val << (i * 8));
    }
    return val;
}

static int32_t read_opt_u24(const uint8_t *data, size_t data_len, size_t *bit_pos) {
    int flag = read_bit(data, data_len, bit_pos);
    if (flag <= 0) return -1;
    return (int32_t)read_u24_bits(data, data_len, bit_pos);
}

static int decode_symbol(const HuffTable *ht, const uint8_t *data, size_t data_len, size_t *bit_pos) {
    uint32_t code = 0;
    for (int len = 1; len <= ht->max_len; len++) {
        int bit = read_bit(data, data_len, bit_pos);
        if (bit < 0) return -1;
        code = (code << 1) | (uint32_t)bit;
        uint32_t fc = ht->levels[len - 1].first_code;
        uint16_t cnt = ht->levels[len - 1].count;
        if (cnt > 0 && code >= fc && code < fc + cnt) {
            uint16_t sym_idx = ht->levels[len - 1].sym_offset + (uint16_t)(code - fc);
            return ht->table_buf[sym_idx];
        }
    }
    return -1;
}

static void gbk_to_utf8_write(FILE *out, const uint8_t *gbk, int len, int normalize_cr) {
    if (len <= 0) return;
    const uint8_t *src = gbk;
    int src_len = len;
    uint8_t *norm = NULL;
    if (normalize_cr) {
        norm = (uint8_t *)malloc((size_t)len * 2);
        if (norm) {
            int j = 0;
            for (int i = 0; i < len; i++) {
                if (gbk[i] == '\r') {
                    norm[j++] = '\r';
                    if (i + 1 >= len || gbk[i + 1] != '\n') norm[j++] = '\n';
                } else {
                    norm[j++] = gbk[i];
                }
            }
            src = norm;
            src_len = j;
        }
    }

    int wlen = MultiByteToWideChar(936, 0, (const char *)src, src_len, NULL, 0);
    if (wlen <= 0) {
        fwrite(src, 1, (size_t)src_len, out);
        free(norm);
        return;
    }
    wchar_t *wbuf = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (wbuf) {
        MultiByteToWideChar(936, 0, (const char *)src, src_len, wbuf, wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
        char *ubuf = (char *)malloc((size_t)ulen + 1);
        if (ubuf) {
            WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, ubuf, ulen, NULL, NULL);
            fwrite(ubuf, 1, (size_t)ulen, out);
            free(ubuf);
        }
        free(wbuf);
    }
    free(norm);
}

static void gbk_to_wide_sanitized(const char *gbk, wchar_t *out, int out_cap) {
    int wlen = MultiByteToWideChar(936, 0, gbk, -1, out, out_cap);
    if (wlen <= 0) {
        wcsncpy(out, L"category", (size_t)(out_cap - 1));
        out[out_cap - 1] = L'\0';
        return;
    }
    for (int i = 0; out[i]; i++) {
        if (wcschr(L"\\/:*?\"<>|", out[i])) out[i] = L'_';
    }
}

static void ensure_dir_w(const wchar_t *path) {
    CreateDirectoryW(path, NULL);
}

static int unpack_dict_v02(FILE *f, uint64_t file_size, uint16_t hdr_size, const wchar_t *out_dir) {
    uint8_t sub_hdr[0x80];
    _fseeki64(f, hdr_size, SEEK_SET);
    if (fread(sub_hdr, 1, 0x80, f) != 0x80) {
        fprintf(stderr, "[Error] Failed to read dictionary sub-header.\n");
        return 1;
    }

    uint64_t base = hdr_size;
    uint32_t entry_count = read_u24(sub_hdr + 0x1D);
    uint64_t ht1_off = base + read_u24(sub_hdr + 0x20);
    uint16_t ht1_len = read_u16(sub_hdr + 0x23);
    uint64_t ht2_off = base + read_u24(sub_hdr + 0x25);
    uint16_t ht2_len = read_u16(sub_hdr + 0x28);
    uint64_t text_idx_off = base + read_u24(sub_hdr + 0x43);
    uint64_t anim_idx_off = base + read_u24(sub_hdr + 0x46);
    uint64_t text_data_off = base + read_u32(sub_hdr + 0x58);
    uint64_t jpg_data_off = base + read_u32(sub_hdr + 0x5C);
    uint64_t mp3_data_off = base + read_u32(sub_hdr + 0x60);

    if (mp3_data_off > file_size) mp3_data_off = file_size;

    printf("Format : dict V0.2\n");
    printf("Entries: %u | Text: 0x%llX | JPEG: 0x%llX | MP3: 0x%llX\n",
           entry_count,
           (unsigned long long)text_data_off,
           (unsigned long long)jpg_data_off,
           (unsigned long long)mp3_data_off);

    wchar_t img_dir[MAX_PATH * 2], audio_dir[MAX_PATH * 2];
    ensure_dir_w(out_dir);
    swprintf(img_dir, MAX_PATH * 2, L"%ls\\images", out_dir);
    swprintf(audio_dir, MAX_PATH * 2, L"%ls\\audio", out_dir);
    ensure_dir_w(img_dir);
    ensure_dir_w(audio_dir);

    uint32_t jpg_cap = 1024;
    uint32_t jpg_count = 0;
    JpgEntry *jpg_map = (JpgEntry *)malloc(jpg_cap * sizeof(JpgEntry));
    _fseeki64(f, (int64_t)jpg_data_off, SEEK_SET);
    uint64_t pos = jpg_data_off;
    while (pos + 4 <= mp3_data_off) {
        uint32_t len = 0;
        if (fread(&len, 4, 1, f) != 1 || len == 0 || pos + 4 + len > mp3_data_off) break;
        uint8_t *buf = (uint8_t *)malloc(len);
        if (!buf || fread(buf, 1, len, f) != len) { free(buf); break; }
        wchar_t out_path[MAX_PATH * 2];
        swprintf(out_path, MAX_PATH * 2, L"%ls\\img_%04u.jpg", img_dir, jpg_count);
        FILE *out = _wfopen(out_path, L"wb");
        if (out) { fwrite(buf, 1, len, out); fclose(out); }
        free(buf);
        if (jpg_count >= jpg_cap) {
            jpg_cap *= 2;
            jpg_map = (JpgEntry *)realloc(jpg_map, jpg_cap * sizeof(JpgEntry));
        }
        jpg_map[jpg_count].rel_off = (uint32_t)(pos - jpg_data_off);
        jpg_map[jpg_count].index = jpg_count;
        jpg_count++;
        pos += 4 + len;
    }
    printf("Extracted JPEGs: %u\n", jpg_count);

    uint32_t mp3_count = 0;
    if (mp3_data_off < file_size) {
        _fseeki64(f, (int64_t)mp3_data_off, SEEK_SET);
        pos = mp3_data_off;
        while (pos + 4 <= file_size) {
            uint32_t len = 0;
            if (fread(&len, 4, 1, f) != 1 || len == 0 || pos + 4 + len > file_size) break;
            uint8_t *buf = (uint8_t *)malloc(len);
            if (!buf || fread(buf, 1, len, f) != len) { free(buf); break; }
            wchar_t out_path[MAX_PATH * 2];
            swprintf(out_path, MAX_PATH * 2, L"%ls\\audio_%02u.mp3", audio_dir, mp3_count);
            FILE *out = _wfopen(out_path, L"wb");
            if (out) { fwrite(buf, 1, len, out); fclose(out); }
            free(buf);
            mp3_count++;
            pos += 4 + len;
        }
    }
    printf("Extracted MP3s : %u\n", mp3_count);

    wchar_t anim_path[MAX_PATH * 2];
    swprintf(anim_path, MAX_PATH * 2, L"%ls\\animation_index.tsv", out_dir);
    FILE *tsv = _wfopen(anim_path, L"wb");
    const uint8_t bom[3] = { 0xEF, 0xBB, 0xBF };
    uint32_t anim_entries = 0;
    if (tsv) {
        fwrite(bom, 1, 3, tsv);
        fprintf(tsv, "entry_index\tframe_count\tframes\r\n");
        _fseeki64(f, (int64_t)anim_idx_off, SEEK_SET);
        pos = anim_idx_off;
        while (pos < text_data_off) {
            uint8_t fc = 0;
            if (fread(&fc, 1, 1, f) != 1) break;
            pos += 1;
            fprintf(tsv, "%u\t%u\t", anim_entries, fc);
            for (uint8_t i = 0; i < fc; i++) {
                uint32_t rel_off = 0;
                if (fread(&rel_off, 4, 1, f) != 1) break;
                pos += 4;
                int idx = find_jpg_index(jpg_map, jpg_count, rel_off);
                if (i > 0) fprintf(tsv, ",");
                if (idx >= 0) fprintf(tsv, "img_%04d.jpg", idx);
                else fprintf(tsv, "OFFSET_0x%X", rel_off);
            }
            fprintf(tsv, "\r\n");
            anim_entries++;
        }
        fclose(tsv);
    }
    printf("Animation index: %u entries\n", anim_entries);

    uint8_t *t1_buf = (uint8_t *)malloc(ht1_len);
    uint8_t *t2_buf = (uint8_t *)malloc(ht2_len);
    _fseeki64(f, (int64_t)ht1_off, SEEK_SET);
    fread(t1_buf, 1, ht1_len, f);
    _fseeki64(f, (int64_t)ht2_off, SEEK_SET);
    fread(t2_buf, 1, ht2_len, f);

    HuffTable ht1, ht2;
    if (parse_huff_table(&ht1, t1_buf, ht1_len) != 0 ||
        parse_huff_table(&ht2, t2_buf, ht2_len) != 0) {
        fprintf(stderr, "[Error] Failed to parse Huffman tables.\n");
        free(t1_buf); free(t2_buf); free(jpg_map);
        return 1;
    }
    free(t1_buf);
    free(t2_buf);

    uint32_t *offsets = (uint32_t *)malloc((size_t)(entry_count + 1) * sizeof(uint32_t));
    _fseeki64(f, (int64_t)text_idx_off, SEEK_SET);
    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t b[3];
        fread(b, 1, 3, f);
        offsets[i] = read_u24(b);
    }
    offsets[entry_count] = (uint32_t)(jpg_data_off - text_data_off);

    wchar_t dict_path[MAX_PATH * 2];
    swprintf(dict_path, MAX_PATH * 2, L"%ls\\dictionary.txt", out_dir);
    FILE *dict_out = _wfopen(dict_path, L"wb");
    uint8_t headword[1024], def[65536];
    uint32_t failed_entries = 0;

    if (dict_out) {
        fwrite(bom, 1, 3, dict_out);
        for (uint32_t e = 0; e < entry_count; e++) {
            uint32_t comp_len = offsets[e + 1] - offsets[e];
            uint8_t *comp = (uint8_t *)malloc(comp_len);
            _fseeki64(f, (int64_t)(text_data_off + offsets[e]), SEEK_SET);
            fread(comp, 1, comp_len, f);

            size_t bit_pos = 16;
            const HuffTable *cur = &ht1;
            int field = 0, read_media = 0;
            int hw_len = 0, def_len = 0;

            while (bit_pos < (size_t)comp_len * 8) {
                if (field == 2 && !read_media) {
                    (void)read_opt_u24(comp, comp_len, &bit_pos);
                    (void)read_opt_u24(comp, comp_len, &bit_pos);
                    read_media = 1;
                }
                int s = decode_symbol(cur, comp, comp_len, &bit_pos);
                if (s < 0) break;
                if (s == 0x02) {
                    cur = (cur == &ht1) ? &ht2 : &ht1;
                    continue;
                }
                if (field == 0) {
                    if (s == 0x09) field = 1;
                    else if (hw_len < 1023) headword[hw_len++] = (uint8_t)s;
                } else if (field == 1) {
                    if (s == 0x09) field = 2;
                } else if (field == 2) {
                    if (s == 0x09) field = 3;
                } else {
                    if (s == 0x0A) break;
                    if (def_len < 65535) def[def_len++] = (uint8_t)s;
                }
            }
            free(comp);
            if (field != 3) failed_entries++;

            fprintf(dict_out, "[%u]\t", e);
            gbk_to_utf8_write(dict_out, headword, hw_len, 0);
            fprintf(dict_out, "\t");
            gbk_to_utf8_write(dict_out, def, def_len, 0);
            fprintf(dict_out, "\r\n");
        }
        fclose(dict_out);
    }

    printf("Dictionary text: %u entries (errors: %u)\n", entry_count, failed_entries);

    free(offsets);
    free(ht1.table_buf);
    free(ht2.table_buf);
    free(jpg_map);
    return 0;
}

static int unpack_dict_add(FILE *f, uint64_t file_size, uint16_t hdr_size, const wchar_t *out_dir) {
    (void)file_size;
    uint8_t sub_hdr[0x98];
    _fseeki64(f, hdr_size, SEEK_SET);
    if (fread(sub_hdr, 1, 0x98, f) != 0x98) {
        fprintf(stderr, "[Error] Failed to read DictAdd sub-header.\n");
        return 1;
    }

    uint64_t base = hdr_size;
    uint32_t entry_count = read_u24(sub_hdr + 0x1D);
    uint64_t ht1_off = base + read_u24(sub_hdr + 0x20);
    uint16_t ht1_len = read_u16(sub_hdr + 0x23);
    uint64_t ht2_off = base + read_u24(sub_hdr + 0x25);
    uint16_t ht2_len = read_u16(sub_hdr + 0x28);
    uint64_t hw_idx_off = base + read_u24(sub_hdr + 0x43);
    uint64_t hw_data_off = base + read_u32(sub_hdr + 0x58);
    uint64_t cat_name_off = base + read_u32(sub_hdr + 0x5C);

    uint64_t sec_base[32];
    for (int i = 0; i < 7; i++) {
        sec_base[i] = base + read_u32(sub_hdr + 0x64 + i * 4);
    }
    for (int i = 7; i < 16; i++) {
        sec_base[i] = base + read_u32(sub_hdr + 0x88 + (i - 7) * 4);
    }

    _fseeki64(f, (int64_t)cat_name_off, SEEK_SET);
    uint8_t cat_count = 0;
    fread(&cat_count, 1, 1, f);
    if (cat_count > 16) cat_count = 16;

    char cat_names_gbk[16][128];
    for (int i = 0; i < cat_count; i++) {
        int pos = 0;
        while (pos < 127) {
            int ch = fgetc(f);
            if (ch == EOF || ch == 0) break;
            cat_names_gbk[i][pos++] = (char)ch;
        }
        cat_names_gbk[i][pos] = '\0';
    }

    printf("Format : DictAdd V1.0\n");
    printf("Entries: %u | Categories: %u\n", entry_count, cat_count);

    uint8_t *t1_buf = (uint8_t *)malloc(ht1_len);
    uint8_t *t2_buf = (uint8_t *)malloc(ht2_len);
    _fseeki64(f, (int64_t)ht1_off, SEEK_SET);
    fread(t1_buf, 1, ht1_len, f);
    _fseeki64(f, (int64_t)ht2_off, SEEK_SET);
    fread(t2_buf, 1, ht2_len, f);

    HuffTable ht1, ht2;
    if (parse_huff_table(&ht1, t1_buf, ht1_len) != 0 ||
        parse_huff_table(&ht2, t2_buf, ht2_len) != 0) {
        fprintf(stderr, "[Error] Failed to parse Huffman tables.\n");
        free(t1_buf); free(t2_buf);
        return 1;
    }
    free(t1_buf);
    free(t2_buf);

    uint32_t *hw_offs = (uint32_t *)malloc(entry_count * sizeof(uint32_t));
    _fseeki64(f, (int64_t)hw_idx_off, SEEK_SET);
    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t b[3];
        fread(b, 1, 3, f);
        hw_offs[i] = read_u24(b);
    }

    wchar_t cat_dir[MAX_PATH * 2];
    ensure_dir_w(out_dir);
    swprintf(cat_dir, MAX_PATH * 2, L"%ls\\categories", out_dir);
    ensure_dir_w(cat_dir);

    const uint8_t bom[3] = { 0xEF, 0xBB, 0xBF };
    wchar_t all_path[MAX_PATH * 2];
    swprintf(all_path, MAX_PATH * 2, L"%ls\\all_entries.txt", out_dir);
    FILE *all_out = _wfopen(all_path, L"wb");
    if (all_out) fwrite(bom, 1, 3, all_out);

    FILE *cat_files[16] = { 0 };
    uint32_t cat_item_counts[16] = { 0 };
    for (int i = 0; i < cat_count; i++) {
        wchar_t wname[128], cpath[MAX_PATH * 2];
        gbk_to_wide_sanitized(cat_names_gbk[i], wname, 128);
        swprintf(cpath, MAX_PATH * 2, L"%ls\\%02d_%ls.txt", cat_dir, i + 1, wname);
        cat_files[i] = _wfopen(cpath, L"wb");
        if (cat_files[i]) fwrite(bom, 1, 3, cat_files[i]);
    }

    uint8_t *dec_buf = (uint8_t *)malloc(131072);
    uint8_t entry_buf[2048];
    uint32_t total_items = 0;

    for (uint32_t e = 0; e < entry_count; e++) {
        _fseeki64(f, (int64_t)(hw_data_off + hw_offs[e]), SEEK_SET);
        uint16_t entry_len = 0;
        if (fread(&entry_len, 2, 1, f) != 1 || entry_len == 0 || entry_len > sizeof(entry_buf)) continue;
        if (fread(entry_buf, 1, entry_len, f) != entry_len) continue;

        int hw_len = 0;
        while (hw_len < entry_len && entry_buf[hw_len] != 0x09) hw_len++;
        if (hw_len >= entry_len) continue;

        size_t bit_pos = (size_t)(hw_len + 1) * 8;
        uint32_t attr_offs[16] = { 0 };
        int has_any = 0;
        for (int a = 0; a < cat_count && bit_pos < (size_t)entry_len * 8; a++) {
            int flag = read_bit(entry_buf, entry_len, &bit_pos);
            if (flag < 0) break;
            if (flag == 1) {
                attr_offs[a] = read_u24_bits(entry_buf, entry_len, &bit_pos);
                if (attr_offs[a] != 0) has_any = 1;
            }
        }
        if (!has_any) continue;

        if (all_out) {
            fprintf(all_out, "=== [%u] ", e);
            gbk_to_utf8_write(all_out, entry_buf, hw_len, 0);
            fprintf(all_out, " ===\r\n");
        }

        for (int a = 0; a < cat_count; a++) {
            if (attr_offs[a] == 0) continue;
            _fseeki64(f, (int64_t)(sec_base[a] + attr_offs[a]), SEEK_SET);
            uint8_t item_cnt = 0;
            if (fread(&item_cnt, 1, 1, f) != 1 || item_cnt == 0) continue;

            uint32_t item_offs[256];
            for (int k = 0; k < item_cnt; k++) {
                uint8_t b[3];
                fread(b, 1, 3, f);
                item_offs[k] = read_u24(b);
            }

            for (int k = 0; k < item_cnt; k++) {
                _fseeki64(f, (int64_t)(sec_base[a] + item_offs[k]), SEEK_SET);
                uint16_t comp_len = 0;
                if (fread(&comp_len, 2, 1, f) != 1 || comp_len == 0) continue;
                uint8_t *comp = (uint8_t *)malloc(comp_len);
                if (!comp || fread(comp, 1, comp_len, f) != comp_len) { free(comp); continue; }

                size_t bp = 0;
                const HuffTable *cur = &ht1;
                int dec_len = 0;
                while (bp < (size_t)comp_len * 8 && dec_len < 131070) {
                    int s = decode_symbol(cur, comp, comp_len, &bp);
                    if (s < 0) break;
                    if (s == 0x02) {
                        cur = (cur == &ht1) ? &ht2 : &ht1;
                        continue;
                    }
                    if (s == 0x0A) break;
                    dec_buf[dec_len++] = (uint8_t)s;
                }
                free(comp);

                if (all_out) {
                    fprintf(all_out, "[");
                    gbk_to_utf8_write(all_out, (const uint8_t *)cat_names_gbk[a], (int)strlen(cat_names_gbk[a]), 0);
                    fprintf(all_out, "] ");
                    gbk_to_utf8_write(all_out, dec_buf, dec_len, 1);
                    fprintf(all_out, "\r\n");
                }
                if (cat_files[a]) {
                    fprintf(cat_files[a], "[");
                    gbk_to_utf8_write(cat_files[a], entry_buf, hw_len, 0);
                    fprintf(cat_files[a], "]\r\n");
                    gbk_to_utf8_write(cat_files[a], dec_buf, dec_len, 1);
                    fprintf(cat_files[a], "\r\n\r\n");
                }
                cat_item_counts[a]++;
                total_items++;
            }
        }
        if (all_out) fprintf(all_out, "\r\n");
    }

    if (all_out) fclose(all_out);
    for (int i = 0; i < cat_count; i++) {
        if (cat_files[i]) fclose(cat_files[i]);
    }
    printf("Decoded %u items across %u categories.\n", total_items, cat_count);

    free(dec_buf);
    free(hw_offs);
    free(ht1.table_buf);
    free(ht2.table_buf);
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(65001);

    if (argc < 2) {
        printf("Usage: noah_dict_unpack.exe <input.bin> [output_dir]\n");
        return 1;
    }

    const wchar_t *bin_path = argv[1];
    wchar_t out_dir[MAX_PATH * 2];
    if (argc >= 3) {
        wcsncpy(out_dir, argv[2], MAX_PATH * 2 - 1);
        out_dir[MAX_PATH * 2 - 1] = L'\0';
    } else {
        wchar_t exe_dir[MAX_PATH * 2];
        DWORD len = GetModuleFileNameW(NULL, exe_dir, MAX_PATH * 2 - 1);
        exe_dir[len] = L'\0';
        wchar_t *exe_sep = wcsrchr(exe_dir, L'\\');
        if (!exe_sep) exe_sep = wcsrchr(exe_dir, L'/');
        if (exe_sep) *exe_sep = L'\0';
        else wcscpy(exe_dir, L".");

        const wchar_t *s1 = wcsrchr(bin_path, L'\\');
        const wchar_t *s2 = wcsrchr(bin_path, L'/');
        const wchar_t *base_name = s1 > s2 ? s1 + 1 : (s2 ? s2 + 1 : bin_path);

        wchar_t stem[MAX_PATH];
        wcsncpy(stem, base_name, MAX_PATH - 1);
        stem[MAX_PATH - 1] = L'\0';
        wchar_t *dot = wcsrchr(stem, L'.');
        if (dot) *dot = L'\0';

        swprintf(out_dir, MAX_PATH * 2, L"%ls\\%ls_extracted", exe_dir, stem);
    }

    FILE *f = _wfopen(bin_path, L"rb");
    if (!f) {
        fwprintf(stderr, L"[Error] Cannot open file: %ls\n", bin_path);
        return 1;
    }

    _fseeki64(f, 0, SEEK_END);
    uint64_t file_size = (uint64_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);

    uint8_t outer_hdr[64];
    if (file_size < 64 || fread(outer_hdr, 1, 64, f) != 64) {
        fprintf(stderr, "[Error] File too small.\n");
        fclose(f);
        return 1;
    }

    uint16_t hdr_size = read_u16(outer_hdr + 0x10);
    if (hdr_size < 0x40 || file_size < (uint64_t)hdr_size + 0x80) {
        fprintf(stderr, "[Error] Invalid header size: 0x%X\n", hdr_size);
        fclose(f);
        return 1;
    }

    wprintf(L"Input : %ls\n", bin_path);
    wprintf(L"Output: %ls\n", out_dir);

    int rc = 1;
    if (memcmp(outer_hdr, "dict", 4) == 0) {
        rc = unpack_dict_v02(f, file_size, hdr_size, out_dir);
    } else if (memcmp(outer_hdr, "DictAdd", 7) == 0) {
        rc = unpack_dict_add(f, file_size, hdr_size, out_dir);
    } else {
        fprintf(stderr, "[Error] Unsupported .bin format (magic: %.8s).\n", outer_hdr);
    }

    fclose(f);
    return rc;
}
