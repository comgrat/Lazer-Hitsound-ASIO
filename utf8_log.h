#pragma once
#include <windows.h>
#include <io.h>
#include <cstdio>

namespace asio {
// Only redirected stdout receives a preamble. Keep interactive consoles and
// stderr untouched; empty stderr must continue to mean no error diagnostics.
inline void writeRedirectedUtf8Preamble(FILE* output) {
    const intptr_t value=_get_osfhandle(_fileno(output));
    if(value==-1) return;
    HANDLE handle=reinterpret_cast<HANDLE>(value);
    DWORD mode=0;
    if(GetConsoleMode(handle,&mode)) return;
    DWORD type=GetFileType(handle);
    if(type!=FILE_TYPE_DISK && type!=FILE_TYPE_PIPE) return;
    if(type==FILE_TYPE_DISK) {
        LARGE_INTEGER zero={},position={};
        if(!SetFilePointerEx(handle,zero,&position,FILE_CURRENT)||position.QuadPart!=0) return;
        LARGE_INTEGER length={};
        if(!GetFileSizeEx(handle,&length)||length.QuadPart!=0) return;
    }
    const unsigned char bom[]={0xef,0xbb,0xbf};
    fwrite(bom,1,sizeof(bom),output);
}
}
