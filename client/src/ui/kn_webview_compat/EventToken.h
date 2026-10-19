#pragma once
// The Windows SDK's EventToken.h (not in MinGW's headers): the event registration token WebView2.h takes
// by pointer. Its layout is the SDK's: one 64-bit value.
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
