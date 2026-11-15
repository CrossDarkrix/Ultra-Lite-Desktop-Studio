// EventToken.h for MinGW-w64 builds.
//
// WebView2.h includes "EventToken.h" (the Windows SDK header name).
// MinGW ships the same definitions as eventtoken.h, but only since
// mingw-w64 12-ish; older MSYS2 header sets (e.g. the one that
// produced "'EventRegistrationToken' has not been declared" on the
// user's machine) lack it or hide it. So define the token here
// exactly as the Windows SDK / WIDL-generated header does, and set
// the __eventtoken_h__ guard so a later include of MinGW's own
// eventtoken.h becomes a no-op instead of a redefinition.
//
// Layout per the SDK: a single __int64 value.
#pragma once
#ifndef __eventtoken_h__
#define __eventtoken_h__
struct EventRegistrationToken {
    long long value;
};
typedef struct EventRegistrationToken EventRegistrationToken;
#endif // __eventtoken_h__
