#pragma once
#include <windows.h>
#include <string>
#include <sstream>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace asio {
struct Settings {
    int buffer = 16, driver = 0, rate = 48000, volume = 50, debug = 0;
};
inline std::string trimSetting(const std::string &value) {
    size_t begin=value.find_first_not_of(" \t\r\n");
    if(begin==std::string::npos) return {};
    return value.substr(begin,value.find_last_not_of(" \t\r\n")-begin+1);
}
inline bool parseSettings(std::string text, Settings &result, std::string &error) {
    if(text.compare(0,3,"\xef\xbb\xbf")==0) text.erase(0,3);
    if(text.find('\0')!=std::string::npos) { error="config.ini must be UTF-8 or ASCII, not UTF-16"; return false; }
    Settings next;
    std::array<const char*,5> names={"Buffer Size","ASIO Driver ID","Sample Rate","Output Volume","Debug Log"};
    std::array<int*,5> values={&next.buffer,&next.driver,&next.rate,&next.volume,&next.debug};
    std::array<bool,5> seen={};
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        line=trimSetting(line);
        if(line.empty()||line[0]=='['||line[0]==';'||line[0]=='#') continue;
        size_t equals=line.find('=');if(equals==std::string::npos) continue;
        std::string key=trimSetting(line.substr(0,equals));
        size_t index=0;while(index<names.size()&&key!=names[index]) ++index;
        if(index==names.size()) continue;
        if(seen[index]) {error="duplicate config key: "+key;return false;}
        std::string value=trimSetting(line.substr(equals+1));
        size_t comment=value.find_first_of(";#");if(comment!=std::string::npos) value=trimSetting(value.substr(0,comment));
        errno=0;char* end=nullptr;long number=strtol(value.c_str(),&end,10);
        if(errno||end==value.c_str()||*end||number<INT_MIN||number>INT_MAX){error="invalid integer: "+key;return false;}
        *values[index]=int(number);seen[index]=true;
    }
    for(size_t i=0;i<seen.size();++i)if(!seen[i]){error="incomplete config, missing: "+std::string(names[i]);return false;}
    if(next.buffer<1||next.buffer>8192||next.driver<0||next.rate<0||next.rate>768000||
       (next.rate!=0&&next.rate<8000)||next.volume<0||next.volume>100||next.debug<0||next.debug>1){error="config value outside supported range";return false;}
    result=next;error.clear();return true;
}
inline bool loadSettings(const std::wstring &path, Settings &result, std::string &error, std::string *snapshot=nullptr) {
    // Do not observe an active in-place writer. Atomic-rename editors can still
    // save successfully while a short snapshot read holds the old file open.
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){error="cannot open config.ini, Windows error="+std::to_string(GetLastError());return false;}
    BY_HANDLE_FILE_INFORMATION before={},after={};
    LARGE_INTEGER length={};
    if(!GetFileInformationByHandle(file,&before)||!GetFileSizeEx(file,&length)||length.QuadPart<0||length.QuadPart>65536){CloseHandle(file);error="config.ini too large or unreadable";return false;}
    std::string text(size_t(length.QuadPart),'\0');DWORD read=0;
    bool ok=ReadFile(file,text.data(),DWORD(text.size()),&read,nullptr)!=FALSE&&read==text.size();
    ok=ok&&GetFileInformationByHandle(file,&after)&&before.nFileSizeHigh==after.nFileSizeHigh&&before.nFileSizeLow==after.nFileSizeLow&&
       CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)==0;
    CloseHandle(file);
    if(!ok){error="config.ini changed during read";return false;}
    if(!parseSettings(text,result,error))return false;
    if(snapshot)*snapshot=text;
    return true;
}
inline std::wstring executableSibling(const wchar_t *name) {
    std::wstring path(32768,L'\0');DWORD length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!length||length>=path.size()) return {};
    path.resize(length);size_t slash=path.find_last_of(L"\\/");
    return slash==std::wstring::npos?std::wstring{}:path.substr(0,slash+1)+name;
}
inline std::string utf8Path(const std::wstring &path) {
    int length=WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),nullptr,0,nullptr,nullptr);
    std::string result(size_t(length),'\0');if(length)WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),result.data(),length,nullptr,nullptr);return result;
}
}
