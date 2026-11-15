#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <new>

namespace asio {
// Diagnostic text must never wait for a pipe/file on a playback or hook thread.
// Fixed capacity; contention/full queues drop diagnostics, never audio events.
class AsyncTextLog {
    struct Line { unsigned short size; char text[1024]; };
    static constexpr unsigned capacity=2048;
    Line* lines=nullptr;
    SRWLOCK lock=SRWLOCK_INIT;
    unsigned head=0,tail=0;
    HANDLE wake=nullptr,thread=nullptr;
    FILE* sink=nullptr;
    bool ownsSink=false;
    std::atomic<bool> accepting{false};
    std::atomic<unsigned long> lost{0};
    static DWORD WINAPI run(void* context) { return static_cast<AsyncTextLog*>(context)->drain(); }
    DWORD drain() {
        char batch[32768];
        for(;;) {
            unsigned bytes=0;
            AcquireSRWLockExclusive(&lock);
            while(head!=tail && bytes+lines[head].size<=sizeof(batch)) {
                memcpy(batch+bytes,lines[head].text,lines[head].size);bytes+=lines[head].size;
                head=(head+1)%capacity;
            }
            bool empty=head==tail;
            ReleaseSRWLockExclusive(&lock);
            if(bytes && sink) { fwrite(batch,1,bytes,sink);fflush(sink); }
            if(!accepting.load(std::memory_order_acquire) && empty) break;
            if(empty) WaitForSingleObject(wake,50);
        }
        if(ownsSink && sink) { fclose(sink); sink=nullptr; }
        return 0;
    }
public:
    // This object and a timed-out sink worker live until process exit. There is
    // deliberately no destructor that could block under the game's loader lock.
    bool start(FILE* output,bool owned=false) {
        if(thread) return false;
        sink=output;ownsSink=owned;
        lines=new(std::nothrow) Line[capacity];
        wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!sink||!lines||!wake) { delete[] lines;lines=nullptr;if(wake)CloseHandle(wake);wake=nullptr;if(owned&&sink)fclose(sink);sink=nullptr;return false; }
        accepting=true;
        thread=CreateThread(nullptr,0,run,this,0,nullptr);
        if(!thread){accepting=false;delete[] lines;lines=nullptr;CloseHandle(wake);wake=nullptr;if(owned)fclose(sink);sink=nullptr;return false;}
        return true;
    }
    bool startFile(const wchar_t* path) { return start(_wfopen(path,L"ab"),true); }
    void write(const char* format,...) {
        if(!accepting.load(std::memory_order_acquire)) return;
        char text[1024];va_list args;va_start(args,format);
        int length=vsnprintf(text,sizeof(text),format,args);va_end(args);
        if(length<=0)return;
        unsigned size=static_cast<unsigned>(length)<sizeof(text)?static_cast<unsigned>(length):sizeof(text)-1;
        if(!TryAcquireSRWLockExclusive(&lock)){lost.fetch_add(1,std::memory_order_relaxed);return;}
        unsigned next=(tail+1)%capacity;
        if(next==head){ReleaseSRWLockExclusive(&lock);lost.fetch_add(1,std::memory_order_relaxed);return;}
        lines[tail].size=static_cast<unsigned short>(size);memcpy(lines[tail].text,text,size);tail=next;
        ReleaseSRWLockExclusive(&lock);SetEvent(wake);
    }
    void writeV(const char* format,va_list args) {
        char line[1024];vsnprintf(line,sizeof(line),format,args);write("%s",line);
    }
    bool stop(DWORD timeout=3000) {
        accepting.store(false,std::memory_order_release);
        if(!thread)return true;
        SetEvent(wake);
        if(WaitForSingleObject(thread,timeout)!=WAIT_OBJECT_0)return false;
        // Do not reclaim the fixed buffer/lock: a producer might have observed
        // accepting=true just before stop. Keeping it avoids a shutdown UAF.
        return true;
    }
    unsigned long dropped()const { return lost.load(std::memory_order_relaxed); }
};
}
