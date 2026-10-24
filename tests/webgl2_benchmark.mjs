// Run Summit's existing scripted, real-time benchmark in an iframe. Node 22+.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import {
  readFile,
  writeFile,
  appendFile,
  mkdir,
  mkdtemp,
  rm,
} from "node:fs/promises";
import { resolve, join, extname } from "node:path";
import { parseArgs } from "node:util";
import assert from "node:assert/strict";

const { values } = parseArgs({
  options: {
    root: { type: "string" },
    output: { type: "string" },
    width: { type: "string", default: "640" },
    height: { type: "string", default: "360" },
    steps: { type: "string", default: "12000" },
    warmup: { type: "string", default: "2" },
    "gpu-timing": { type: "boolean", default: false },
    "cpu-profile": { type: "boolean", default: false },
    "gl-calls": { type: "boolean", default: false },
    capture: { type: "boolean", default: false },
    trace: { type: "boolean", default: false },
    "minimal-context": { type: "boolean", default: false },
    "vulkan-compositor": { type: "boolean", default: false },
    headless: { type: "boolean", default: false },
    window: { type: "string", default: "app" },
    "disable-gpu-vsync": { type: "boolean", default: false },
    software: { type: "boolean", default: false },
  },
});
assert(values.root && values.output, "--root and --output are required");
assert(
  ["app", "browser"].includes(values.window),
  "--window must be app or browser",
);
const appWindow = !values.headless && values.window === "app";
const root = resolve(values.root),
  output = resolve(values.output);
const width = Number(values.width),
  height = Number(values.height);
assert(Number.isInteger(width) && width >= 160 && width <= 4096);
assert(Number.isInteger(height) && height >= 90 && height <= 4096);
await mkdir(output, { recursive: true });
const args = [
  "--benchmark",
  "/benchmark.csv",
  "--script",
  "shelter",
  "--quality",
  "core",
  "--scenery",
  "compact",
  "--look",
  "morning",
  "--freeze-time",
  "--seed",
  "1",
  "--mute",
  "--no-settings",
  "--benchmark-warmup",
  values.warmup,
  "--max-steps",
  values.steps,
];
const query = new URLSearchParams(args.map((arg) => ["arg", arg]));
const server = createServer(async (req, res) => {
  if (req.url === "/favicon.ico") {
    res.writeHead(204).end();
    return;
  }
  if (req.url === "/embed.html") {
    res.setHeader("Content-Type", "text/html");
    res.end(
      `<!doctype html><style>html,body{margin:0;width:100%;height:100%;overflow:hidden}iframe{display:block;border:0;width:${width}px;height:${height}px}</style><iframe src="/index.html?${query}" allow="autoplay; fullscreen"></iframe>`,
    );
    return;
  }
  const path = resolve(
    root,
    "." + new URL(req.url, "http://localhost").pathname,
  );
  if (!path.startsWith(root + "/")) {
    res.writeHead(403).end();
    return;
  }
  try {
    const data = await readFile(path);
    res.setHeader(
      "Content-Type",
      {
        ".html": "text/html",
        ".js": "text/javascript",
        ".wasm": "application/wasm",
      }[extname(path)] || "application/octet-stream",
    );
    res.end(data);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const profile = await mkdtemp("/tmp/kin-webgl2-benchmark-");
const flags = [
  ...(values.headless ? ["--headless=new"] : ["--ozone-platform=x11"]),
  "--no-sandbox",
  "--disable-dev-shm-usage",
  ...(values["disable-gpu-vsync"] ? ["--disable-gpu-vsync"] : []),
  ...(values.software
    ? ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
    : [
        "--use-angle=vulkan",
        ...(values["vulkan-compositor"] ? ["--enable-features=Vulkan"] : []),
        "--ignore-gpu-blocklist",
      ]),
  "--remote-debugging-port=0",
  `--user-data-dir=${profile}`,
  "--no-first-run",
  "--no-default-browser-check",
];
const launch = appWindow
  ? [`--app=about:blank`, `--window-size=${width},${height}`]
  : ["about:blank"];
const chrome = spawn("/usr/bin/google-chrome", [...flags, ...launch], {
  stdio: ["ignore", "ignore", "pipe"],
});
let chromeLog = "",
  socket;
const errors = [],
  consoleLines = [];
chrome.stderr.on("data", (data) => (chromeLog += data));
const delay = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(fn, label, ms = 30000) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (errors.length) throw Error(JSON.stringify(errors));
    const result = await fn();
    if (result) return result;
    await delay(100);
  }
  throw Error("Timed out: " + label);
}
try {
  const port = await until(async () => {
    if (chrome.exitCode !== null) throw Error(chromeLog);
    try {
      return Number(
        (await readFile(join(profile, "DevToolsActivePort"), "utf8")).split(
          "\n",
        )[0],
      );
    } catch {
      return false;
    }
  }, "Chrome startup");
  const targets = await (
    await fetch(`http://127.0.0.1:${port}/json/list`)
  ).json();
  socket = new WebSocket(
    targets.find((t) => t.type === "page").webSocketDebuggerUrl,
  );
  await new Promise((r, j) => {
    socket.onopen = r;
    socket.onerror = j;
  });
  let id = 0;
  const pending = new Map();
  let completeTrace;
  const traceDone = new Promise((resolve) => (completeTrace = resolve));
  socket.onmessage = (event) => {
    const m = JSON.parse(event.data);
    if (m.id) {
      const p = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) p?.reject(Error(JSON.stringify(m.error)));
      else p?.resolve(m.result);
    }
    if (m.method === "Runtime.exceptionThrown")
      errors.push(m.params.exceptionDetails);
    if (m.method === "Tracing.tracingComplete") completeTrace(m.params);
    if (m.method === "Runtime.consoleAPICalled") {
      const text = m.params.args.map((a) => a.value ?? a.description).join(" ");
      consoleLines.push(text);
      if (m.params.type === "error" && !/\b(INFO|DEBUG|TRACE|WARN) /.test(text))
        errors.push(text);
    }
  };
  const call = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const next = ++id;
      const timer = setTimeout(() => {
        pending.delete(next);
        reject(Error("CDP timeout: " + method));
      }, 15000);
      pending.set(next, {
        resolve: (r) => {
          clearTimeout(timer);
          resolve(r);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      });
      socket.send(JSON.stringify({ id: next, method, params }));
    });
  const evaluate = async (expression) => {
    const r = await call("Runtime.evaluate", {
      expression,
      returnByValue: true,
    });
    if (r.exceptionDetails) throw Error(JSON.stringify(r.exceptionDetails));
    return r.result.value;
  };
  await call("Runtime.enable");
  await call("Page.enable");
  await call("Page.bringToFront");
  await call("Emulation.setDeviceMetricsOverride", {
    width: appWindow ? width : Math.max(1024, width),
    height: appWindow ? height : Math.max(768, height),
    deviceScaleFactor: 1,
    mobile: false,
  });
  await call("Page.addScriptToEvaluateOnNewDocument", {
    source: `(()=>{
    window.kinBenchmarkGLCalls={};window.kinBenchmarkGLUploadBytes=0;
    if(${values["gl-calls"]})for(const name of ['uniform1i','uniform1f','uniform1iv','uniform4f','bufferData',
      'bindVertexArray','drawElementsInstanced','drawArraysInstanced','drawElements','drawArrays','activeTexture','bindTexture']){
      const original=WebGL2RenderingContext.prototype[name];
      WebGL2RenderingContext.prototype[name]=function(...args){
        window.kinBenchmarkGLCalls[name]=(window.kinBenchmarkGLCalls[name]||0)+1;
        if(name==='bufferData')window.kinBenchmarkGLUploadBytes+=typeof args[1]==='number'?args[1]:
          args.length>4?args[4]*args[1].BYTES_PER_ELEMENT:args[1].byteLength;
        return Reflect.apply(original,this,args);
      };
    }
    if(${values["minimal-context"]}){
      const getContext=HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext=function(type,options){
        return Reflect.apply(getContext,this,[type,type==='webgl2'?{...options,alpha:false,depth:false,stencil:false,antialias:false}:options]);
      };
    }
    const request=window.requestAnimationFrame.bind(window);let last=0;
    window.kinBenchmarkFrames=[];window.kinBenchmarkGPU=[];
    let pending=[],timer,disjointCount=0;
    window.requestAnimationFrame=callback=>request(time=>{
      const calls=${values["gl-calls"]}?{...window.kinBenchmarkGLCalls}:null;
      const uploadBytes=window.kinBenchmarkGLUploadBytes;
      const game=window.Module?.kinSummit,frame=game?.frames;
      const gl=${values["gpu-timing"]}&&window.Module?.kinReady?Module.canvas.getContext('webgl2'):null;
      let query;
      if(gl&&!gl.isContextLost()){
        timer??=gl.getExtension('EXT_disjoint_timer_query_webgl2');
        if(timer){
          const disjoint=gl.getParameter(timer.GPU_DISJOINT_EXT);
          if(disjoint){for(const p of pending)gl.deleteQuery(p.query);pending=[];disjointCount++;}
          else while(pending.length&&gl.getQueryParameter(pending[0].query,gl.QUERY_RESULT_AVAILABLE)){
            const p=pending.shift();window.kinBenchmarkGPU.push({frame:p.frame,step:p.step,ms:gl.getQueryParameter(p.query,gl.QUERY_RESULT)/1e6});gl.deleteQuery(p.query);
          }
          if(pending.length<8){query=gl.createQuery();gl.beginQuery(timer.TIME_ELAPSED_EXT,query);}
        }
      }
      const started=performance.now();
      try{return callback(time);}finally{
        const now=window.Module?.kinSummit;
        if(query){gl.endQuery(timer.TIME_ELAPSED_EXT);pending.push({query,frame:now?.frames,step:now?.steps});}
        if(now&&now.frames!==frame){
          const sample={frame:now.frames,step:now.steps,phase:now.phase,callbackMs:performance.now()-started,intervalMs:last?time-last:0,startedMs:started,rafTimestampMs:time,rafDelayMs:started-time,fillMs:now.fillMs,renderMs:now.renderMs,uiMs:now.uiMs};
          if(calls){sample.glCalls=Object.fromEntries(Object.entries(window.kinBenchmarkGLCalls).map(([name,n])=>[name,n-(calls[name]||0)]));sample.glUploadBytes=window.kinBenchmarkGLUploadBytes-uploadBytes;}
          window.kinBenchmarkFrames.push(sample);
          if(window.kinBenchmarkFrames.length>30000)throw Error('Benchmark frame limit exceeded');
        }
        last=time;window.kinBenchmarkGPUDisjoints=disjointCount;
      }
    });
  })()`,
  });
  await call("Page.navigate", {
    url: `http://127.0.0.1:${server.address().port}/embed.html`,
  });
  await until(
    () =>
      evaluate(
        `document.querySelector('iframe')?.contentWindow.Module?.kinReady`,
      ),
    "first completed frame",
    60000,
  );
  const info = await evaluate(
    `(()=>{const w=document.querySelector('iframe').contentWindow,gl=w.Module.canvas.getContext('webgl2'),ext=gl.getExtension('WEBGL_debug_renderer_info');return {renderer:gl.getParameter(ext?ext.UNMASKED_RENDERER_WEBGL:gl.RENDERER),timer:!!gl.getExtension('EXT_disjoint_timer_query_webgl2'),attributes:gl.getContextAttributes(),width:gl.drawingBufferWidth,height:gl.drawingBufferHeight,error:gl.getError(),isolated:w.crossOriginIsolated,visible:w.document.visibilityState,focused:w.document.hasFocus()}})()`,
  );
  assert.equal(info.error, 0);
  assert.equal(info.width, width);
  assert.equal(info.height, height);
  assert(
    values.software || !/(SwiftShader|llvmpipe|software)/i.test(info.renderer),
    "Hardware GPU required: " + info.renderer,
  );
  const version = await (
    await fetch(`http://127.0.0.1:${port}/json/version`)
  ).json();
  const inspector = new WebSocket(version.webSocketDebuggerUrl);
  const system = await new Promise((resolve, reject) => {
    let gpu;
    const timer = setTimeout(() => {
      inspector.close();
      reject(Error("GPU system info timeout"));
    }, 5000);
    inspector.onopen = () =>
      inspector.send(JSON.stringify({ id: 1, method: "SystemInfo.getInfo" }));
    inspector.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id === 1) {
        if (m.error) {
          clearTimeout(timer);
          inspector.close();
          reject(Error(JSON.stringify(m.error)));
          return;
        }
        gpu = m.result;
        inspector.send(
          JSON.stringify({
            id: 2,
            method: "Browser.getWindowForTarget",
            params: { targetId: targets.find((t) => t.type === "page").id },
          }),
        );
      }
      if (m.id === 2) {
        clearTimeout(timer);
        inspector.close();
        resolve({
          ...gpu,
          browserWindow: m.error ? { error: m.error } : m.result,
        });
      }
    };
    inspector.onerror = (e) => {
      clearTimeout(timer);
      reject(e);
    };
  });
  await writeFile(
    join(output, "gpu-system-info.json"),
    JSON.stringify(system, null, 2),
  );
  const viewport = (await call("Page.getLayoutMetrics")).cssVisualViewport;
  await writeFile(
    join(output, "environment.json"),
    JSON.stringify(
      {
        info,
        args,
        flags: [...flags, ...launch],
        viewport,
        browserWindow: system.browserWindow,
        gpuTiming: values["gpu-timing"],
        presentation: values.headless ? "offscreen" : "windowed",
        window: values.headless ? null : values.window,
        contextOverride: values["minimal-context"],
        trace: values.trace,
      },
      null,
      2,
    ),
  );
  if (values.capture) {
    const shot = await call("Page.captureScreenshot", { format: "png" });
    await writeFile(
      join(output, "initial.png"),
      Buffer.from(shot.data, "base64"),
    );
  }
  console.log(JSON.stringify({ started: true, ...info }));
  if (values["cpu-profile"]) {
    await call("Profiler.enable");
    await call("Profiler.setSamplingInterval", { interval: 250 });
    await call("Profiler.start");
  }
  if (values.trace)
    await call("Tracing.start", {
      transferMode: "ReturnAsStream",
      streamCompression: "gzip",
      traceConfig: {
        recordMode: "recordContinuously",
        traceBufferSizeInKb: 65536,
        includedCategories: [
          "toplevel",
          "cc",
          "gpu",
          "viz",
          "benchmark",
          "latencyInfo",
          "devtools.timeline",
          "disabled-by-default-devtools.timeline.frame",
          "renderer.scheduler",
        ],
      },
    });
  const result = await until(
    () =>
      evaluate(
        `document.querySelector('iframe').contentWindow.Module.kinSummitBenchmark`,
      ),
    "scripted traversal",
    240000,
  );
  if (values["cpu-profile"]) {
    const { profile } = await call("Profiler.stop");
    await writeFile(
      join(output, "gameplay.cpuprofile"),
      JSON.stringify(profile),
    );
    await call("Profiler.disable");
  }
  if (values.trace) {
    await call("Tracing.end");
    const trace = await traceDone;
    assert(!trace.dataLossOccurred, "Trace buffer overflowed; shorten the run");
    const path = join(output, "browser-trace.json.gz");
    await writeFile(path, "");
    while (true) {
      const part = await call("IO.read", {
        handle: trace.stream,
        size: 1024 * 1024,
      });
      await appendFile(
        path,
        part.base64Encoded ? Buffer.from(part.data, "base64") : part.data,
      );
      if (part.eof) break;
    }
    await call("IO.close", { handle: trace.stream });
  }
  await writeFile(join(output, "benchmark.csv"), result.csv);
  await writeFile(
    join(output, "benchmark.csv.json"),
    JSON.stringify(result.metadata, null, 2),
  );
  const diagnostics = await evaluate(
    `(()=>{const w=document.querySelector('iframe').contentWindow;return {frames:w.kinBenchmarkFrames,gpu:w.kinBenchmarkGPU,disjoints:w.kinBenchmarkGPUDisjoints}})()`,
  );
  await writeFile(join(output, "frames.json"), JSON.stringify(diagnostics));
  assert.equal(result.metadata.quality, "core");
  assert.equal(result.metadata.scenery, "compact");
  assert.equal(result.metadata.samples, 1);
  assert.equal(result.metadata.width, width);
  assert.equal(result.metadata.height, height);
  assert.equal(result.metadata.falls, 0);
  assert.equal(result.metadata.deaths, 0);
  assert(
    result.metadata.completed || result.metadata.steps >= Number(values.steps),
    "Bot must reach the requested step budget",
  );
  const [header, ...lines] = result.csv
    .trim()
    .split("\n")
    .map((line) => line.split(","));
  const column = (name) => header.indexOf(name);
  assert(lines.length > 10, "Benchmark must export frame samples");
  for (const row of lines.filter((row) => row[column("measured")] === "1")) {
    assert.equal(Number(row[column("shadow_cascades")]), 2);
    assert.equal(Number(row[column("skinned_copies")]), 1);
    assert.equal(Number(row[column("skin_matrices")]), 20);
  }
  assert.deepEqual(errors, []);
  console.log(JSON.stringify(result.metadata));
} finally {
  await writeFile(join(output, "chrome.log"), chromeLog);
  await writeFile(join(output, "console.log"), consoleLines.join("\n"));
  socket?.close();
  chrome.kill("SIGTERM");
  await Promise.race([
    new Promise((r) =>
      chrome.exitCode !== null || chrome.signalCode
        ? r()
        : chrome.once("exit", r),
    ),
    delay(2000),
  ]);
  server.closeAllConnections();
  server.close();
  await rm(profile, {
    recursive: true,
    force: true,
    maxRetries: 5,
    retryDelay: 100,
  });
}
