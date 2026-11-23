// v10.0.7's profanity Filter (chat_filter.h). The list is chat_filter_words.inc; the matching follows the JS
// regexes byte by byte: \w = [A-Za-z0-9_], \b between a \w and a non-\w byte (or the text's ends), the "i" flag
// folds ASCII only (every list word is ASCII). A UTF-8 byte >= 0x80 is a non-word byte, as every UTF-16 unit
// outside ASCII is a non-word unit to the JS regexes, so the pieces split at the same places.
#include "chat_filter.h"

#include <string.h>

static const char *const FILTER_WORDS[] = {
#include "chat_filter_words.inc"
};

static int is_word(const unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static unsigned char fold(const unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char) (c - 'A' + 'a') : c;
}

// \b at `at` of text[0..len)
static int boundary(const unsigned char *text, const size_t len, const size_t at) {
    const int before = at > 0 && is_word(text[at - 1]);
    const int after = at < len && is_word(text[at]);
    return before != after;
}

// new RegExp("\\b" + w.replace(/(\W)/g, "\\$1") + "\\b", "gi").test(text): every non-word character of w escaped,
// so w is matched literally between two word boundaries
static int word_matches(const char *word, const unsigned char *text, const size_t len) {
    const size_t n = strlen(word);
    if (n > len) return 0;
    for (size_t at = 0; at + n <= len; at++) {
        if (!boundary(text, len, at) || !boundary(text, len, at + n)) continue;
        size_t i = 0;
        while (i < n && fold((unsigned char) word[i]) == fold(text[at + i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

int chat_filter_is_profane(const char *text, const size_t len) {
    // list.filter(w => !exclude.includes(w.toLowerCase()) && re.test(text)).length > 0 (exclude is empty)
    for (size_t i = 0; i < sizeof(FILTER_WORDS) / sizeof(FILTER_WORDS[0]); i++)
        if (word_matches(FILTER_WORDS[i], (const unsigned char *) text, len)) return 1;
    return 0;
}

// replaceWord: piece.replace(/[^a-zA-Z0-9|\$|\@]|\^/g, "").replace(/\w/g, "*")
static void replace_word(const unsigned char *piece, const size_t len, char *out, const size_t size, size_t *at) {
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = piece[i];
        const int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '|' || c == '$' || c == '@';
        if (!keep || *at + 1 >= size) continue;
        out[(*at)++] = is_word(c) ? '*' : (char) c;
    }
}

int chat_filter_clean(const char *in, char *out, const size_t size) {
    if (!out || !size) return 0;
    out[0] = 0;
    const unsigned char *text = (const unsigned char *) (in ? in : "");
    const size_t len = strlen((const char *) text);
    int any = 0; // /\b/.exec(text): null without a single word character
    for (size_t i = 0; i < len && !any; i++) any = is_word(text[i]);
    if (!any) return 0;
    // text.split(/\b/): the pieces between consecutive word boundaries inside (0, len)
    size_t at = 0, start = 0;
    for (size_t pos = 1; pos <= len; pos++) {
        if (pos < len && !boundary(text, len, pos)) continue;
        const size_t n = pos - start;
        if (chat_filter_is_profane((const char *) text + start, n)) {
            replace_word(text + start, n, out, size, &at);
        } else {
            const size_t room = size - 1 - at;
            const size_t copy = n < room ? n : room;
            memcpy(out + at, text + start, copy);
            at += copy;
        }
        start = pos;
    }
    out[at] = 0;
    return 1;
}
