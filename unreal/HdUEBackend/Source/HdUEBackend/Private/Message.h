#pragma once
#include "CoreMinimal.h"

// Text into an ABI struct's char message[N]: UTF-8, truncated to fit, always NUL-terminated.
template <SIZE_T N> void HdueCopyMessage(char (&Dest)[N], const FString& Text) {
    FTCHARToUTF8 Utf8(*Text);
    const int32 Count = FMath::Min(Utf8.Length(), int32(N - 1));
    FMemory::Memcpy(Dest, Utf8.Get(), Count);
    Dest[Count] = 0;
}
