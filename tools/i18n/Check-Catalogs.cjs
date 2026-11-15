const fs=require('fs'),path=require('path');const root=path.resolve(__dirname,'../..');
const codes=['zh-CN','zh-TW','en','fr','es'];
function parse(code){const file=path.join(root,'locales',code+'.lang');const bytes=fs.readFileSync(file);const text=new TextDecoder('utf-8',{fatal:true}).decode(bytes);if(text.includes('\0'))throw Error(code+' NUL');let out={};for(const line of text.split(/\r?\n/)){if(!line||line[0]==='#')continue;let e=line.indexOf('=');if(e<1)throw Error(code+' malformed line');let k=line.slice(0,e),v=line.slice(e+1);if(k in out)throw Error(code+' duplicate '+k);out[k]=v;}return out;}
let catalogs=Object.fromEntries(codes.map(c=>[c,parse(c)])),reference=catalogs.en,checks=0;
for(const code of codes){let c=catalogs[code];if(JSON.stringify(Object.keys(c).sort())!==JSON.stringify(Object.keys(reference).sort()))throw Error(code+' keyset mismatch');for(const k of Object.keys(reference)){const placeholders=s=>[...s.matchAll(/\{\d+\}/g)].map(m=>m[0]).sort().join(',');if(placeholders(c[k])!==placeholders(reference[k]))throw Error(code+' placeholders '+k);if(!c[k])throw Error(code+' empty '+k);checks++;}console.log(code+': '+Object.keys(c).length+' keys; placeholders OK');}
// Every statically referenced key must be present in the canonical catalog.
const files=['config.cpp','launcher.cpp','osu!asio_sound.cpp','tools/device/AsioDeviceCheck.cpp','managed/StartupHook.cs','managed/LiveEntry.cs','managed/AudioContract.cs','managed/Start-Standalone.ps1'];
for(const file of files){let s=fs.readFileSync(path.join(root,file),'utf8');for(const m of s.matchAll(/(?:ui::text|UiText\.Get)\("([\w.]+)"|Get-AsioText '([\w.]+)'/g)){let key=m[1]||m[2];if(!(key in reference))throw Error(file+' missing '+key);checks++;}}
console.log('Catalog checks passed='+checks);
