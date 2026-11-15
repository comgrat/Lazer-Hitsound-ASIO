#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <initializer_list>
#include <sstream>
#include <cstdio>
#include <cstring>
#include "i18n_english.h"

namespace asio::i18n {
inline const char* codes[]={"zh-CN","zh-TW","en","fr","es"};
inline const char* names[]={"简体中文","繁體中文","English","Français","Español"};
inline std::wstring root;
inline std::string language="en";
inline std::unordered_map<std::string,std::string> catalog=englishCatalog();
inline std::string trim(const std::string& text){size_t a=text.find_first_not_of(" \t\r\n");if(a==std::string::npos)return {};return text.substr(a,text.find_last_not_of(" \t\r\n")-a+1);}
inline bool valid(const std::string& code){for(auto c:codes)if(code==c)return true;return false;}
inline std::wstring widen(const std::string& value){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);if(n<=0)return {};std::wstring out(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),n);return out;}
inline bool readUtf8(const std::wstring& path,std::string& out){
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 LARGE_INTEGER size={};if(!GetFileSizeEx(file,&size)||size.QuadPart<0||size.QuadPart>262144){CloseHandle(file);return false;}
 out.assign(size_t(size.QuadPart),'\0');DWORD got=0;bool ok=ReadFile(file,out.data(),DWORD(out.size()),&got,nullptr)&&got==out.size();CloseHandle(file);
 if(!ok||out.find('\0')!=std::string::npos)return false;
 if(out.compare(0,3,"\xef\xbb\xbf")==0)out.erase(0,3);
 return out.empty()||MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,out.data(),int(out.size()),nullptr,0)>0;
}
inline std::unordered_map<std::string,std::string> parseCatalog(const std::string& text){
 std::unordered_map<std::string,std::string> result;std::istringstream lines(text);std::string line;
 while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;size_t eq=line.find('=');if(eq==std::string::npos||!eq)continue;result.emplace(trim(line.substr(0,eq)),line.substr(eq+1));}return result;
}
inline bool select(const std::string& code){
 if(!valid(code))return false;
 auto next=englishCatalog();std::string text;
 if(readUtf8(root+L"\\locales\\"+widen(code)+L".lang",text))for(auto &pair:parseCatalog(text))if(next.count(pair.first))next[pair.first]=pair.second;
 language=code;catalog=std::move(next);return true;
}
inline void initialize(const std::wstring& directory={}){
 root=directory;
 if(root.empty()){wchar_t path[32768];DWORD n=GetModuleFileNameW(nullptr,path,32768);if(n&&n<32768){std::wstring full(path,n);size_t slash=full.find_last_of(L"\\/");if(slash!=std::wstring::npos)root=full.substr(0,slash);}}
 std::string saved;
 if(readUtf8(root+L"\\language.ini",saved)){for(auto &p:parseCatalog(saved))if(p.first=="Language"&&valid(trim(p.second))){select(trim(p.second));return;}}
 select("en");
}
inline bool hasSavedLanguage(){std::string saved;if(!readUtf8(root+L"\\language.ini",saved))return false;for(auto &p:parseCatalog(saved))if(p.first=="Language"&&valid(trim(p.second)))return true;return false;}
inline std::string text(const char* key,std::initializer_list<std::string> args={}){
 auto found=catalog.find(key);std::string source=found==catalog.end()?std::string(key):found->second;
 // Substitute once in the original template; braces in file paths/device names
 // supplied as arguments cannot be interpreted as additional placeholders.
 std::string result;size_t i=0;
 while(i<source.size()){
  if(source[i]=='{'){size_t end=source.find('}',i+1);if(end!=std::string::npos){std::string index=source.substr(i+1,end-i-1);if(!index.empty()&&index.find_first_not_of("0123456789")==std::string::npos){unsigned n=0;for(char c:index){n=n*10+unsigned(c-'0');if(n>1024)break;}if(n<args.size()){result+=*(args.begin()+n);i=end+1;continue;}}}}
  result+=source[i++];
 }return result;
}
inline bool save(const std::string& code){
 if(!valid(code))return false;
 std::wstring temporary=root+L"\\language.ini.tmp",target=root+L"\\language.ini";
 std::string data="[Interface]\r\nLanguage = "+code+"\r\n";
 HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;bool ok=WriteFile(file,data.data(),DWORD(data.size()),&written,nullptr)&&written==data.size();if(ok)ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file);
 if(!ok||!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
 return select(code);
}
inline void printLanguages(){for(unsigned i=0;i<5;++i)printf("  %u. %s (%s)\n",i+1,names[i],codes[i]);}
inline bool chooseLanguage(){
 puts(text("language.menu").c_str());printLanguages();
 for(;;){printf("%s",text("language.prompt").c_str());fflush(stdout);char input[64];if(!fgets(input,sizeof(input),stdin))return false;std::string value=trim(input);int selected=value.empty()?2:value.size()==1&&value[0]>='1'&&value[0]<='5'?value[0]-'1':-1;if(selected<0){puts(text("language.selection_invalid").c_str());continue;}if(!save(codes[selected])){fprintf(stderr,"%s\n",text("language.save_failed").c_str());return false;}puts(text("language.saved",{names[selected]}).c_str());return true;}
}
// Returns -1 when no language-only command was provided, otherwise an exit code.
inline int languageCommand(int argc,char** argv){
 if(argc==2&&!strcmp(argv[1],"--list-languages")){printLanguages();return 0;}
 if(argc==2&&!strcmp(argv[1],"--current-language")){puts(language.c_str());return 0;}
 if(argc>=2&&!strcmp(argv[1],"--language")){
  if(argc==2)return chooseLanguage()?0:2;
  if(argc!=3||!valid(argv[2])){fprintf(stderr,"%s\n",text("language.invalid",{argc>2?argv[2]:""}).c_str());return 2;}
  if(!save(argv[2])){fprintf(stderr,"%s\n",text("language.save_failed").c_str());return 1;}
  unsigned index=0;while(index<5&&language!=codes[index])++index;puts(text("language.saved",{names[index<5?index:2]}).c_str());return 0;
 }return -1;
}
}
