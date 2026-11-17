// Real file bytes and real append/reopen paths; no game or audio device.
#include "../async_text_log.h"
#include "../utf8_log.h"
#include <string>
#include <fstream>
#include <iterator>
#include <memory>

static int checks=0,failures=0;
static void check(bool value,const char* label) {
    ++checks;printf("%s %s\n",value?"PASS":"FAIL",label);if(!value)++failures;
}
static std::string bytes(const std::wstring& path) {
    std::ifstream input(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),{});
}
static void seed(const std::wstring& path,const std::string& value) {
    std::ofstream output(path,std::ios::binary);output.write(value.data(),value.size());
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--emit")) {
        asio::writeRedirectedUtf8Preamble(stdout);
        puts("[ASIO_READY] 中文状态");return 0;
    }
    wchar_t temp[MAX_PATH]={};GetTempPathW(MAX_PATH,temp);
    const std::wstring root=std::wstring(temp)+L"AsioLogProbe_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
    check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"create isolated fixture directory");
    const std::string bom="\xef\xbb\xbf";
    for(int mode=0;mode<3;++mode) {
        const auto path=root+L"\\append-"+std::to_wstring(mode)+L".log";
        const std::string original=mode==0?"":mode==1?bom+"已有内容\r\n":"旧UTF-8文件\r\n";
        if(mode) seed(path,original);
        for(int pass=0;pass<2;++pass) {
            // Logger state intentionally lives to process exit, as in production.
            auto* log=new asio::AsyncTextLog;
            check(log->startFile(path.c_str()),"open append logger");
            check(!log->startFile((root+L"\\must-not-create.log").c_str()),"duplicate start rejected before file I/O");
            check(log->stop(),"join file writer");
        }
        check(bytes(path)==(mode==0?bom:original),"one BOM on new file; existing bytes unchanged after reopen");
    }
    check(GetFileAttributesW((root+L"\\must-not-create.log").c_str())==INVALID_FILE_ATTRIBUTES,"rejected duplicate start creates no file");
    const auto missing=root+L"\\missing\\log";
    auto* log=new asio::AsyncTextLog;
    check(!log->startFile(missing.c_str()),"invalid file path fails cleanly");
    check(log->stop(),"failed logger stops cleanly");
    const auto redirected=root+L"\\stdout.log";
    FILE* output=_wfopen(redirected.c_str(),L"ab");
    check(output!=nullptr,"open redirected stdout fixture");
    if(output) {
        asio::writeRedirectedUtf8Preamble(output);
        fputs("[ASIO_READY] 中文状态\n",output);fflush(output);
        asio::writeRedirectedUtf8Preamble(output);fclose(output);
        check(bytes(redirected)==bom+"[ASIO_READY] 中文状态\n","stdout preamble precedes status exactly once");
    }
    const auto timed=root+L"\\timed.log";
    auto* timedLog=new asio::AsyncTextLog;
    check(timedLog->startFile(timed.c_str()),"open timestamp fixture");
    auto before=GetTickCount64();timedLog->write("first\nsecond\n");
    check(timedLog->stop(),"timestamp logger stops");auto after=GetTickCount64();
    const auto content=bytes(timed);unsigned long long a=0,b=0;
    int parsed=sscanf(content.c_str()+3,"[t=%llu] first\n[t=%llu] second",&a,&b);
    check(parsed==2 && a==b && a>=before && b<=after,"each logical line shares boot-relative monotonic timestamp");
    printf("DiagnosticLogProbe checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
