// Uses an existing Chromium and Node 22's WebSocket; no browser package download.
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
const root=path.resolve(import.meta.dirname,'../..');
const chrome=process.env.ZT_CHROME || '/home/mohit/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome';
const origin=process.env.ZT_GRAFANA || 'http://127.0.0.1:13000';
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const listener=net.createServer();await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));const port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
const browser=spawn(chrome,['--headless=new','--no-sandbox','--disable-dev-shm-usage',`--remote-debugging-port=${port}`,`--user-data-dir=${root}/artifacts/grafana-browser-profile`],{stdio:'ignore'});
let ws;
try {
  let page;
  for(let i=0;i<100;i++) {
    try {page=await (await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(origin+'/login')}`,{method:'PUT'})).json();break;} catch {await sleep(200);}
  }
  if(!page)throw new Error('Chromium startup failed');
  ws=new WebSocket(page.webSocketDebuggerUrl);await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
  let seq=0;const pending=new Map();
  ws.onmessage=event=>{const msg=JSON.parse(event.data);if(msg.id){const entry=pending.get(msg.id);pending.delete(msg.id);if(entry){clearTimeout(entry.timer);msg.error?entry.reject(new Error(JSON.stringify(msg.error))):entry.resolve(msg.result);}}};
  function command(method,params={}) {return new Promise((resolve,reject)=>{const id=++seq;const timer=setTimeout(()=>{pending.delete(id);reject(new Error('CDP timeout: '+method));},20000);pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params}));});}
  await command('Page.enable');await command('Runtime.enable');
  await command('Emulation.setDeviceMetricsOverride',{width:1600,height:1200,deviceScaleFactor:1,mobile:false});
  await sleep(1500);
  const password=fs.readFileSync(root+'/artifacts/secrets/grafana-password','utf8').trim();
  const login=await command('Runtime.evaluate',{expression:`fetch('/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(${JSON.stringify({user:'admin',password})})}).then(r=>r.status)`,awaitPromise:true,returnByValue:true});
  if(login.result.value!==200)throw new Error('Grafana login failed');
  await command('Page.navigate',{url:origin+'/d/zt-proxy/zt-proxy-local-kubernetes?from=now-10m&to=now&kiosk'});
  await sleep(15000);
  const content=await command('Runtime.evaluate',{expression:'document.body.innerText',returnByValue:true});
  if(!content.result.value.includes('Policy version per pod'))throw new Error('Dashboard did not load');
  const shot=await command('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
  fs.writeFileSync(root+'/bench/results/k8s/grafana.png',Buffer.from(shot.data,'base64'));
  console.log('Captured live Grafana dashboard.');
} finally {if(ws)ws.close();browser.kill('SIGTERM');}
