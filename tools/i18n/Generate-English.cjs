// Generate the native English fallback from the shared UTF-8 catalog.
const fs=require('fs'),path=require('path');
const root=path.resolve(__dirname,'../..');
const pairs=[];const seen=new Set();
for(const raw of fs.readFileSync(path.join(root,'locales/en.lang'),'utf8').replace(/^\uFEFF/,'').split(/\r?\n/)){
 if(!raw.trim()||raw.startsWith('#'))continue;
 const split=raw.indexOf('=');if(split<1)throw new Error('Invalid catalog line: '+raw);
 const key=raw.slice(0,split),value=raw.slice(split+1);
 if(seen.has(key))throw new Error('Duplicate key '+key);seen.add(key);pairs.push([key,value]);
}
const content='// Generated from locales/en.lang by tools/i18n/Generate-English.cjs.\n#pragma once\n#include <unordered_map>\n#include <string>\nnamespace asio::i18n {\ninline std::unordered_map<std::string,std::string> englishCatalog(){return {\n'+pairs.map(([k,v])=>'    {'+JSON.stringify(k)+','+JSON.stringify(v)+'},').join('\n')+'\n};}\n}\n';
fs.writeFileSync(path.join(root,'i18n_english.h'),content,'utf8');
console.log('Generated native English fallback: '+pairs.length+' keys');
