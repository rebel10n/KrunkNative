#include "jsonw.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jsonw_number(const double value, char *out, const size_t size) {
    if (!isfinite(value)) {
        snprintf(out, size, "null");
        return;
    }
    if (value == 0.0) { // -0 too
        snprintf(out, size, "0");
        return;
    }
    // shortest %.Ne that reads back exactly (ECMAScript Number::toString picks the shortest k)
    char e[40];
    int precision = 1;
    for (; precision <= 17; precision++) {
        snprintf(e, sizeof(e), "%.*e", precision - 1, value);
        if (strtod(e, NULL) == value) break;
    }
    // e = [-]d[.ddd]e[+-]xx
    const char *s = e;
    int negative = 0;
    if (*s == '-') {
        negative = 1;
        s++;
    }
    char digits[24];
    int k = 0;
    for (; *s && *s != 'e'; s++) {
        if (*s >= '0' && *s <= '9' && k < (int) sizeof(digits) - 1) digits[k++] = *s;
    }
    digits[k] = 0;
    const int exponent = *s == 'e' ? atoi(s + 1) : 0;
    while (k > 1 && digits[k - 1] == '0') digits[--k] = 0;
    const int n = exponent + 1; // decimal point position

    char buf[64];
    size_t len = 0;
    if (negative) buf[len++] = '-';
    if (k <= n && n <= 21) {
        memcpy(buf + len, digits, (size_t) k);
        len += (size_t) k;
        for (int i = 0; i < n - k; i++) buf[len++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(buf + len, digits, (size_t) n);
        len += (size_t) n;
        buf[len++] = '.';
        memcpy(buf + len, digits + n, (size_t) (k - n));
        len += (size_t) (k - n);
    } else if (-6 < n && n <= 0) {
        buf[len++] = '0';
        buf[len++] = '.';
        for (int i = 0; i < -n; i++) buf[len++] = '0';
        memcpy(buf + len, digits, (size_t) k);
        len += (size_t) k;
    } else {
        buf[len++] = digits[0];
        if (k > 1) {
            buf[len++] = '.';
            memcpy(buf + len, digits + 1, (size_t) (k - 1));
            len += (size_t) (k - 1);
        }
        len += (size_t) snprintf(buf + len, sizeof(buf) - len, "e%c%d", n - 1 >= 0 ? '+' : '-', abs(n - 1));
    }
    buf[len] = 0;
    snprintf(out, size, "%s", buf);
}

static void put_string(Sb *sb, const char *s) {
    sb_putc(sb, '"');
    for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\b': sb_puts(sb, "\\b"); break;
            case '\f': sb_puts(sb, "\\f"); break;
            case '\n': sb_puts(sb, "\\n"); break;
            case '\r': sb_puts(sb, "\\r"); break;
            case '\t': sb_puts(sb, "\\t"); break;
            default:
                if (*p < 0x20) sb_printf(sb, "\\u%04x", *p);
                else sb_putc(sb, (char) *p);
        }
    }
    sb_putc(sb, '"');
}

static int is_scalar(const cJSON *item) {
    return !cJSON_IsArray(item) && !cJSON_IsObject(item);
}

static int short_array(const cJSON *array) {
    int n = 0;
    for (const cJSON *c = array->child; c; c = c->next) {
        if (!is_scalar(c) || cJSON_IsString(c)) return 0;
        if (++n > 8) return 0;
    }
    return 1;
}

static void newline(Sb *sb, const int depth) {
    sb_putc(sb, '\n');
    for (int i = 0; i < depth; i++) sb_puts(sb, "  ");
}

static void put(Sb *sb, const cJSON *item, const JsonwStyle style, const int depth) {
    if (!item) {
        sb_puts(sb, "null");
        return;
    }
    switch (item->type & 0xff) {
        case cJSON_False: sb_puts(sb, "false"); return;
        case cJSON_True: sb_puts(sb, "true"); return;
        case cJSON_NULL: sb_puts(sb, "null"); return;
        case cJSON_Number: {
            char num[64];
            jsonw_number(item->valuedouble, num, sizeof(num));
            sb_puts(sb, num);
            return;
        }
        case cJSON_String: put_string(sb, item->valuestring); return;
        case cJSON_Raw: sb_puts(sb, item->valuestring ? item->valuestring : "null"); return;
        case cJSON_Array: {
            sb_putc(sb, '[');
            if (!item->child) {
                sb_putc(sb, ']');
                return;
            }
            const int inline_items = style == JSONW_COMPACT || (style == JSONW_PRETTY && short_array(item));
            for (const cJSON *c = item->child; c; c = c->next) {
                if (!inline_items) newline(sb, depth + 1);
                put(sb, c, style, depth + 1);
                if (c->next) sb_putc(sb, ',');
            }
            if (!inline_items) newline(sb, depth);
            sb_putc(sb, ']');
            return;
        }
        case cJSON_Object: {
            sb_putc(sb, '{');
            if (!item->child) {
                sb_putc(sb, '}');
                return;
            }
            for (const cJSON *c = item->child; c; c = c->next) {
                if (style != JSONW_COMPACT) newline(sb, depth + 1);
                put_string(sb, c->string);
                sb_puts(sb, style != JSONW_COMPACT ? ": " : ":");
                put(sb, c, style, depth + 1);
                if (c->next) sb_putc(sb, ',');
            }
            if (style != JSONW_COMPACT) newline(sb, depth);
            sb_putc(sb, '}');
            return;
        }
        default: sb_puts(sb, "null"); return;
    }
}

char *jsonw_print(const cJSON *item, const JsonwStyle style) {
    Sb sb = {0};
    put(&sb, item, style, 0);
    return sb_take(&sb);
}
