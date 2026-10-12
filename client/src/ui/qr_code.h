// QR Code encoder (ISO/IEC 18004) for v7.1.4's 2FA popup: index.js twoFAres draws the
// otpauth:// URI with QrCode.toDataURL (src/shim/qrcode.js). Byte mode, versions 1-10,
// error correction L or M, automatic or fixed mask. No dependencies; GL-free (unit tested by
// client/tests/qr_code_test.c against goldens from python-qrcode).
#pragma once
#include <stddef.h>

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (17 + 4 * QR_MAX_VERSION) // 57 modules

typedef enum { QR_ECC_L = 0, QR_ECC_M = 1 } QrEcc;

typedef struct QrCode {
    int version;                              // 1..QR_MAX_VERSION
    int size;                                 // 17 + 4 * version
    int mask;                                 // 0..7
    unsigned char dark[QR_MAX_SIZE][QR_MAX_SIZE]; // [y][x], 1 = dark module
} QrCode;

// Smallest version that holds `len` bytes at `ecc` (mask -1 = lowest penalty). 0 ok, -1 too long.
int qr_encode(const unsigned char *data, size_t len, QrEcc ecc, int mask, QrCode *out);

// An SVG image of the code (black on white, a 4-module quiet zone, one <path>), as text in out.
// Returns the length, -1 when cap is too small.
int qr_svg(const QrCode *qr, char *out, size_t cap);
