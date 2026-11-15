import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { Cdp, sleep, waitForTarget } from './cdp.mjs';
import { buildGraph, clickSelector, newDoc, runAndWait, select } from './page.mjs';

const root = path.resolve(import.meta.dirname, '../..');
const output = path.join(root, 'build/p6-browser-evidence');
await fs.mkdir(output, { recursive: true });
const profile = await fs.mkdtemp(path.join(output, 'browser-'));
const browserPath =
  process.env.LYFLOW_BROWSER ?? 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const port = Number(process.env.LYFLOW_P6_CDP_PORT ?? 9349);
const host = process.env.LYFLOW_P6_HOST ?? 'http://127.0.0.1:5178/';
const browser = spawn(
  browserPath,
  [
    '--headless=new',
    '--disable-gpu',
    '--no-first-run',
    '--no-default-browser-check',
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${profile}`,
    '--window-size=1700,1100',
    host,
  ],
  { windowsHide: true, stdio: 'ignore' },
);
let cdp;
const errors = [],
  evidence = [];
const record = (name, details) => {
  evidence.push({ name, ...details });
  console.log(name, JSON.stringify(details));
};
const screenshot = async (name) => {
  const result = await cdp.send('Page.captureScreenshot', { format: 'png' });
  await fs.writeFile(path.join(output, `${name}.png`), Buffer.from(result.data, 'base64'));
};
const change = async (selector, value) => {
  await cdp.eval(
    `const element = document.querySelector(${JSON.stringify(selector)}); if(!element) throw new Error('missing control'); element.value = ${JSON.stringify(value)}; element.dispatchEvent(new Event('change', { bubbles:true })); return true;`,
  );
};
const fixture = async (kind) =>
  cdp.eval(`
  const bridge = window.__lyflow, {graph,execution,ui,manifest}=bridge.stores;
  ui.getState().setAutoRun(false);
  const id='18446744073709551614';
  const pose=(dim,extras={})=>({kind:'Record',type:'vision.Pose'+dim+'D.v1',data:{id:'零件甲',frame:'camera',unit:dim===2?'px':'m',position:dim===2?[3,3]:[0,0,2],size:dim===2?[4,2]:[0.2,0.4,0.3],...(dim===2?{angle:Math.PI/4}:{rotation:[0,0,0,1]}),...extras}});
  const rec=(type,data)=>({kind:'Record',type,data});
  const stat=(port,type,value,n=1,sourceCloudId)=>({port,type,value,elementCount:n,byteSize:0,...(sourceCloudId?{sourceCloudId}:{})});
  const group=rec('cloud.Groups.v1',{sourceCloudId:id,pointCount:3,frame:'camera',unit:'m',groups:[{id:'甲',offset:0,count:2},{id:'乙',offset:2,count:1}]});
  const imageValue={kind:'Image',width:8,height:8,channels:1,depth:'u8'};
  const normalValue={kind:'Image',width:8,height:8,channels:3,depth:'f32'};
  let infos={};
  if(${JSON.stringify(kind)}==='groups') infos={parent:[stat('cloud','PointCloud',undefined,3,id)],fixture:[stat('cloud','PointCloud',undefined,1,'7'),stat('groups','Record',group),stat('indices','Indices',undefined,3)]};
  else if(${JSON.stringify(kind)}==='scene') infos={fixture:[stat('scene','Record',rec('visualization.SceneSpec.v1',{schemaVersion:1,frame:{id:'profile',unit:'mm',axes:['u','h'],yDirection:'down'},layers:[{id:'circle',frame:{id:'profile',unit:'mm',axes:['u','h'],yDirection:'down'},kind:'circle',center:[2,3],radius:1,color:'#5ec9d8'},{id:'name',frame:{id:'profile',unit:'mm',axes:['u','h'],yDirection:'down'},kind:'text',position:[2,3],text:'中文<script>',color:'#5ec9d8'}]}))]};
  else infos={fixture:[stat('image','Image',imageValue,64),stat('normal','Image',normalValue,64),stat('poses','Record',pose(2)),stat('maskInfo','Record',rec('vision.Masks2D.v1',{frame:'camera',unit:'px',width:8,height:8,layout:'NHW',items:[{id:'零件甲',index:0,hasMask:true}]})),stat('masks','Tensor',{kind:'Tensor',shape:[1,8,8],count:64},64),stat('quality','Record',rec('image.ModelProjection.v1',{outputs:{normal:{encoding:'camera-unit-vector-xyz'}}}))]};
  if(${JSON.stringify(kind)}==='image3d') infos.fixture=[...infos.fixture.filter(o=>!['poses','maskInfo','masks'].includes(o.port)),stat('poses','Record',pose(3,{frame:'world',position:[0,0,4],size:[2,2,0.4]})),stat('camera','Record',rec('vision.CameraCalib.v1',{frame:'camera',unit:'m',imageSize:[8,8],cameraMatrix:[4,0,3,0,4,3,0,0,1],distortion:[]}))];
  if(${JSON.stringify(kind)}==='cloudimage') infos={parent:[stat('cloud','PointCloud',undefined,3,id)],fixture:[stat('image','Image',imageValue,64),stat('groups','Record',group),stat('indices','Indices',undefined,3),stat('camera','Record',rec('vision.CameraCalib.v1',{frame:'camera',unit:'m',imageSize:[8,8],cameraMatrix:[4,0,3,0,4,3,0,0,1],distortion:[]}))]};
  const cloud=(which,cap)=>{const total=which==='parent'?3:1, stride=cap>0?Math.max(1,Math.ceil(total/cap)):1, count=Math.ceil(total/stride), buf=new ArrayBuffer(40+count*12),v=new DataView(buf); v.setUint32(0,0x4350594c,true);v.setUint32(4,count,true);v.setUint32(8,total,true);new Float32Array(buf,16,6).set([0,0,1,2,0,1]); for(let n=0;n<count;n++)new Float32Array(buf,40+n*12,3).set([n*stride,0,1]);return buf;};
  bridge.transport.getRuntimeCapabilities=async()=>({schemaVersion:1,resultBindings:true,cloudSampling:'ordered-stride-v1'});
  bridge.transport.getRunBindings=async(runId)=>({schemaVersion:1,type:'lyflow.RunBindings.v1',runId,entries:Object.entries(infos).flatMap(([nodeId,outputs])=>outputs.map(o=>({...o,nodeId,recordType:o.value?.type}))),missing:[]});
  bridge.transport.getOutputInfo=async(run,node)=>infos[node]??[];
  bridge.transport.getOutputCloud=async(run,node,port,cap)=>cloud(node,cap);
  bridge.transport.getOutputIndices=async()=>{const b=new ArrayBuffer(36),h=new DataView(b);h.setUint32(0,0x5849594c,true);h.setUint32(4,3,true);h.setUint32(8,3,true);h.setBigUint64(16,BigInt(id),true);new Int32Array(b,24,3).set([0,2,1]);return b;};
  bridge.transport.getOutputTensor=async()=>{const b=new ArrayBuffer(56+64*4),h=new DataView(b);h.setUint32(0,0x4e54594c,true);h.setUint32(4,3,true);h.setUint32(12,64,true);h.setBigUint64(24,64n,true);new BigInt64Array(b,32,3).set([1n,8n,8n]);const p=new Float32Array(b,56,64);for(let y=2;y<6;y++)for(let x=2;x<6;x++)p[y*8+x]=1;return b;};
  bridge.transport.getOutputImage=async(run,node,port)=>{const normal=port==='normal',channels=normal?3:1,bytes=normal?4:1,b=new ArrayBuffer(48+64*channels*bytes),h=new DataView(b);[0x4d49594c,8,8,channels,bytes,0,8,8,0,8,8*channels*bytes,0].forEach((n,i)=>h.setUint32(i*4,n,true));if(normal){const p=new Float32Array(b,48);for(let n=1;n<64;n++)p.set([0,0,-1],n*3);}else new Uint8Array(b,48).fill(80);return b;};
  const op={id:'fixture.op',version:'1.0.0',label:'结果查看夹具',category:'test',inputs:[],outputs:infos.fixture.map(o=>({name:o.port,type:o.type,required:true})),params:[]};
  const m=manifest.getState();manifest.setState({operatorsById:new Map([...m.operatorsById,[op.id,op]]),coreInfo:{...m.coreInfo,generation:(m.coreInfo?.generation??0)+1}});
  graph.getState().loadDoc({schemaVersion:1,id:'fixture',nodes:[{id:'fixture',op:op.id,params:{},ui:{position:{x:0,y:0}}}],edges:[]},null);
  execution.setState({runId:'fixture-'+${JSON.stringify(kind)},resultRunId:'fixture-'+${JSON.stringify(kind)},runStatus:'ok',preview:false,nodes:new Map([['fixture',{state:'done',errors:[],stats:{outputs:infos.fixture}}]])});
  ui.getState().setViewerContentPick(null); ui.getState().setSelection(['fixture'], []);
  return true;
`);
try {
  const target = await waitForTarget(port, {
    match: (target) => target.url.startsWith(host),
    timeoutMs: 30000,
  });
  cdp = await Cdp.connect(target.webSocketDebuggerUrl);
  await cdp.send('Runtime.enable');
  cdp.on('Runtime.exceptionThrown', (event) =>
    errors.push(event.exceptionDetails?.exception?.description ?? event.exceptionDetails?.text),
  );
  await cdp.send('Browser.setDownloadBehavior', {
    behavior: 'allow',
    downloadPath: output,
    eventsEnabled: true,
  });
  await cdp.waitFor("window.__lyflow?.stores.manifest.getState().status === 'ready'", {
    timeoutMs: 30000,
  });
  await cdp.eval('window.__lyflow.stores.ui.getState().setAutoRun(false);return true;');
  await newDoc(cdp);
  const ids = await buildGraph(
    cdp,
    [
      {
        key: 'a',
        op: 'pose.construct_3d',
        params: { id: '真实工件', frame: 'camera', position: [0, 0, 1], size: [0.1, 0.2, 0.3] },
      },
    ],
    [],
  );
  const nativeRun = await runAndWait(cdp, () => cdp.eval('return window.__lyflow.run();'));
  await select(cdp, ids.a);
  await cdp.waitFor("document.querySelector('[data-testid=viewer-pose-controls]') !== null");
  assert.equal(await cdp.eval("return document.querySelector('.viewer').dataset.overlay;"), '1');
  await clickSelector(
    cdp,
    '[data-testid=viewer-pose-controls] input[type=checkbox]:nth-of-type(1)',
  );
  await screenshot('native-pose3d');
  record('native CLI-backed Pose3D view', { nativeRun });
  const modelGraph = JSON.parse(
    await fs.readFile(path.join(root, 'examples/p6-model-viewer.lyflow.json'), 'utf8'),
  );
  await cdp.eval(
    `window.__lyflow.stores.graph.getState().loadDoc(${JSON.stringify(modelGraph)}, ${JSON.stringify(path.join(root, 'examples/p6-model-viewer.lyflow.json'))});return true;`,
  );
  const modelRun = await runAndWait(cdp, () => cdp.eval('return window.__lyflow.run();'));
  await select(cdp, 'render');
  await cdp.waitFor("document.querySelector('[aria-label=图像输出端口]')?.options.length===5");
  for (const imagePort of ['mask', 'edge_mask', 'depth', 'normal']) {
    await change('[aria-label=图像输出端口]', imagePort);
    await cdp.waitFor(
      "document.querySelector('[data-testid=viewer-image-export]')?.disabled===false",
    );
    await sleep(200);
    assert.deepEqual(
      await cdp.eval(
        "const c=document.querySelector('[data-testid=viewer-image-canvas]');return [c.width,c.height];",
      ),
      [1280, 960],
    );
    await screenshot(`native-model-${imagePort}`);
  }
  await clickSelector(cdp, '[data-testid=viewer-image-export]');
  record('native STL sampling and four scientific render ports', { modelRun });
  await fixture('groups');
  await cdp.waitFor("document.querySelector('[aria-label=点云分组来源]')?.options.length===2");
  await change('[aria-label=点云分组来源]', 'fixture:groups');
  await cdp.waitFor("document.querySelector('[aria-label=点云组身份]')?.options.length===3");
  await change('[aria-label=点云组身份]', '乙');
  await cdp.waitFor("document.querySelector('.viewer__count')?.textContent.startsWith('1 / 3')");
  record(
    'lossless group source and identity selection',
    await cdp.eval(
      "return {count:document.querySelector('.viewer__count')?.textContent,controls:document.querySelector('[data-testid=viewer-group-controls]').textContent};",
    ),
  );
  await screenshot('group-selection');
  await fixture('scene');
  await cdp.waitFor("document.querySelector('[data-testid=result-scene] img')?.naturalWidth>0");
  assert.ok(
    await cdp.eval(
      "return document.querySelector('[data-testid=result-scene]').textContent.includes('profile');",
    ),
  );
  await clickSelector(cdp, '[aria-label=放大结构化结果]');
  await clickSelector(cdp, '[data-testid=result-scene] a[download]');
  await screenshot('structured-scene');
  record('SceneSpec SVG and zoom', { pass: true });
  await fixture('image');
  await cdp.waitFor("document.querySelector('[aria-label=叠加工件]')?.options.length===2");
  await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await change('[aria-label=叠加工件]', 'fixture:poses');
  await change('[aria-label=图像坐标系]', 'camera');
  await cdp.waitFor("document.querySelector('img[alt=工件与点云投影]')?.naturalWidth>0");
  assert.equal(
    await cdp.eval(
      "return document.querySelector('[data-testid=image-result-tools] [role=status]').textContent;",
    ),
    '',
  );
  await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await screenshot('pose2d-mask');
  await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await clickSelector(cdp, '[data-testid=viewer-image-export]');
  await change('[aria-label=图像输出端口]', 'normal');
  await cdp.waitFor(
    "document.querySelector('[data-testid=viewer-image-pane]')?.dataset.source==='output' && document.querySelector('[aria-label=图像输出端口]')?.value==='normal'",
  );
  await sleep(600);
  const normal = await cdp.eval(
    "const c=document.querySelector('[data-testid=viewer-image-canvas]');const p=c.getContext('2d').getImageData(0,0,2,1).data; return [...p];",
  );
  assert.deepEqual(normal.slice(0, 4), [0, 0, 0, 255]);
  assert.deepEqual(normal.slice(4, 8), [128, 128, 0, 255]);
  record('native image port selection and scientific normal mapping', { normal });
  await screenshot('normal-image');
  await fixture('image3d');
  await cdp.waitFor("document.querySelector('[aria-label=叠加工件]')?.options.length===2");
  if (!(await cdp.eval("return document.querySelector('[data-testid=image-result-tools]').open;")))
    await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await change('[aria-label=叠加工件]', 'fixture:poses');
  await cdp.waitFor("document.querySelector('[aria-label=投影相机]')?.options.length===2");
  await change('[aria-label=图像坐标系]', 'camera');
  await change('[aria-label=投影相机]', 'fixture:camera');
  await cdp.waitFor(
    "document.querySelector('[data-testid=image-result-tools] [role=status]')?.textContent.includes('explicit')",
  );
  const transform = {
    fromFrame: 'world',
    toFrame: 'camera',
    matrix: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  };
  await cdp.eval(
    `const input=document.querySelector('[aria-label=投影外参]');Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input, ${JSON.stringify(JSON.stringify(transform))});input.dispatchEvent(new Event('input',{bubbles:true}));return true;`,
  );
  await cdp.waitFor(
    "document.querySelector('[data-testid=image-result-tools] [role=status]')?.textContent==='' && document.querySelector('img[alt=工件与点云投影]')?.naturalWidth>0",
  );
  await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await screenshot('pose3d-camera-projection');
  record('camera overlay requires and consumes explicit cross-frame transform', { pass: true });
  await fixture('cloudimage');
  await cdp.waitFor(
    "document.querySelector('[data-testid=viewer-camera] option[value=image]')?.disabled===false",
  );
  await change('[data-testid=viewer-camera]', 'image');
  await cdp.waitFor("document.querySelector('[aria-label=投影点云组]')?.options.length===2");
  if (!(await cdp.eval("return document.querySelector('[data-testid=image-result-tools]').open;")))
    await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await change('[aria-label=图像坐标系]', 'camera');
  await change('[aria-label=投影相机]', 'fixture:camera');
  await change('[aria-label=投影点云组]', 'fixture:groups');
  await change('[aria-label=投影云组身份]', '乙');
  await cdp.waitFor(
    "document.querySelector('[data-testid=image-result-tools] [role=status]')?.textContent==='点云投影上限 10 万点，按身份着色' && document.querySelector('img[alt=工件与点云投影]')?.naturalWidth>0",
  );
  const cloudSvg = await cdp.eval(
    "return await (await fetch(document.querySelector('img[alt=工件与点云投影]').src)).text();",
  );
  assert.ok(cloudSvg.includes('cx="7.5"') && (cloudSvg.match(/<circle/g) ?? []).length === 1);
  await clickSelector(cdp, '[data-testid=image-result-tools] summary');
  await screenshot('cloud-group-image-projection');
  record('cloud identity, group selection and calibrated image projection', { pass: true });
  await sleep(400);
  const files = await fs.readdir(output);
  assert.ok(files.some((name) => name.endsWith('.svg')));
  assert.ok(files.some((name) => name.startsWith('image-result-') && name.endsWith('.png')));
  assert.deepEqual(errors, []);
  await fs.writeFile(
    path.join(output, 'evidence.json'),
    JSON.stringify({ evidence, errors, files }, null, 2),
  );
} catch (error) {
  if (cdp) {
    console.log(
      'browser failure',
      JSON.stringify(
        await cdp
          .eval(
            'return {href:location.href,body:document.body.innerText,bridge:!!window.__lyflow,manifest:window.__lyflow?.stores.manifest.getState().error};',
          )
          .catch(String),
      ),
    );
    console.log('browser errors', JSON.stringify(errors));
    await screenshot('failure').catch(() => {});
  }
  throw error;
} finally {
  cdp?.close();
  await new Promise((resolve) =>
    spawn('taskkill', ['/pid', String(browser.pid), '/T', '/F'], {
      windowsHide: true,
      stdio: 'ignore',
    }).once('exit', resolve),
  );
  if (!path.resolve(profile).startsWith(path.resolve(output) + path.sep))
    throw new Error('Browser profile escaped the verification directory');
  await fs
    .rm(profile, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 })
    .catch(() => {});
}
