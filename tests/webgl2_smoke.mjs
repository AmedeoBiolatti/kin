// Browser integration check with Node 22+ and an installed Chrome; no npm deps.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile, writeFile, mkdir, mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve, join, extname } from "node:path";
import { parseArgs } from "node:util";
import assert from "node:assert/strict";

const { values } = parseArgs({
  options: {
    root: { type: "string" },
    output: { type: "string", default: "/tmp/kin-webgl2-smoke" },
    chrome: { type: "string", default: "/usr/bin/google-chrome" },
    mode: { type: "string", default: "hex" },
    headed: { type: "boolean", default: false },
    "deny-storage": { type: "boolean", default: false },
    profile: { type: "boolean", default: false },
    hardware: { type: "boolean", default: false },
    "cpu-profile": { type: "boolean", default: false },
    "gl-calls": { type: "boolean", default: false },
  },
});
assert(values.root, "--root must name the extracted browser bundle");
assert(!values["gl-calls"] || values.profile, "--gl-calls requires --profile");
assert(
  ["hex", "3d"].includes(values.mode),
  "--mode must be hex or 3d",
);
const root = resolve(values.root),
  output = resolve(values.output);
await mkdir(output, { recursive: true });
const profile = await mkdtemp(join(tmpdir(), "kin-webgl2-chrome-"));
if (values["deny-storage"]) {
  // A real browser policy denial, without replacing IndexedDB or the game's storage code.
  await mkdir(join(profile, "Default"));
  await writeFile(
    join(profile, "Default", "Preferences"),
    JSON.stringify({
      profile: { default_content_setting_values: { cookies: 2 } },
    }),
  );
}
const errors = [];
const consoleLines = [];
const server = createServer(async (req, res) => {
  if (req.url === "/favicon.ico") {
    res.writeHead(204).end();
    return;
  }
  if (req.url === "/embed.html") {
    res.setHeader("Content-Type", "text/html");
    res.end(
      `<!doctype html><style>body{margin:0}iframe{border:0;width:960px;height:600px}</style><iframe src="/index.html" allow="autoplay; fullscreen" allowfullscreen></iframe>`,
    );
    return;
  }
  const pathname = decodeURIComponent(
    new URL(req.url, "http://localhost").pathname,
  );
  const file = resolve(
    root,
    "." + (pathname === "/" ? "/index.html" : pathname),
  );
  if (!file.startsWith(root + "/")) {
    res.writeHead(403).end();
    return;
  }
  try {
    const data = await readFile(file);
    res.setHeader(
      "Content-Type",
      {
        ".html": "text/html",
        ".js": "text/javascript",
        ".wasm": "application/wasm",
      }[extname(file)] || "application/octet-stream",
    );
    res.end(data);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const chrome = spawn(
  values.chrome,
  [
    ...(values.headed ? ["--ozone-platform=x11"] : ["--headless=new"]),
    "--no-sandbox",
    "--disable-dev-shm-usage",
    ...(values.hardware
      ? []
      : ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"]),
    "--remote-debugging-port=0",
    `--user-data-dir=${profile}`,
    "--no-first-run",
    "--no-default-browser-check",
    "about:blank",
  ],
  { stdio: ["ignore", "ignore", "pipe"] },
);
let chromeLog = "";
chrome.stderr.on("data", (data) => {
  chromeLog += data;
});
let socket;
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(fn, label, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (errors.length) throw new Error(label + ": " + JSON.stringify(errors));
    const result = await fn();
    if (result) return result;
    await delay(100);
  }
  throw new Error("Timed out: " + label);
}
try {
  const port = await until(async () => {
    if (chrome.exitCode !== null)
      throw new Error("Chrome exited: " + chromeLog);
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
    targets.find((target) => target.type === "page").webSocketDebuggerUrl,
  );
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });
  let id = 0;
  const pending = new Map();
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const handler = pending.get(message.id);
      pending.delete(message.id);
      if (message.error)
        handler.reject(new Error(JSON.stringify(message.error)));
      else handler.resolve(message.result);
    } else if (message.method === "Runtime.exceptionThrown")
      errors.push(message.params.exceptionDetails);
    if (message.method === "Runtime.consoleAPICalled") {
      const line = message.params.args
        .map((arg) => arg.value || arg.description)
        .join(" ");
      consoleLines.push(line);
    }
    if (
      message.method === "Log.entryAdded" &&
      message.params.entry.level === "error"
    ) {
      const text = message.params.entry.text;
      if (!text.includes("favicon.ico")) errors.push(text);
    } else if (
      message.method === "Runtime.consoleAPICalled" &&
      message.params.type === "error"
    ) {
      const text = message.params.args
        .map((arg) => arg.value || arg.description)
        .join(" ");
      // SDL's Emscripten log sink sends every severity to console.error.
      if (!/\b(INFO|DEBUG|TRACE|WARN) /.test(text)) errors.push(text);
    }
  };
  function call(method, params = {}) {
    const next = ++id;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => {
          pending.delete(next);
          reject(new Error("CDP timeout: " + method));
        },
        15000,
      );
      pending.set(next, {
        resolve: (result) => {
          clearTimeout(timer);
          resolve(result);
        },
        reject: (error) => {
          clearTimeout(timer);
          reject(error);
        },
      });
      socket.send(JSON.stringify({ id: next, method, params }));
    });
  }
  async function evaluate(expression) {
    const result = await call("Runtime.evaluate", {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (result.exceptionDetails)
      throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async function focusWindow() {
    await call("Page.bringToFront");
    if (values.headed) {
      // Pointer lock checks native window focus as well as DOM focus. Xvfb has no window manager.
      await new Promise((resolve, reject) => {
        const focus = spawn(
          "xdotool",
          [
            "search",
            "--sync",
            "--onlyvisible",
            "--limit",
            "1",
            "--pid",
            String(chrome.pid),
            "windowfocus",
          ],
          { timeout: 15000 },
        );
        focus.on("error", reject);
        focus.on("exit", (code) =>
          code === 0
            ? resolve()
            : reject(new Error("xdotool could not focus test Chrome")),
        );
      });
    }
    await evaluate(
      `document.querySelector('iframe')?.contentWindow.Module?.canvas?.focus()`,
    );
    await delay(100);
  }
  await call("Runtime.enable");
  await call("Log.enable");
  await call("Page.enable");
  await focusWindow();
  await call("Emulation.setFocusEmulationEnabled", { enabled: true });
  await call("Emulation.setDeviceMetricsOverride", {
    width: 1024,
    height: 768,
    deviceScaleFactor: 1,
    mobile: false,
  });
  if (values.profile) {
    await call("Page.addScriptToEvaluateOnNewDocument", {
      source: `(() => {
      window.kinGLCalls = {};
      if (${values["gl-calls"]}) {
        for (const name of ['activeTexture', 'bindSampler', 'bindTexture', 'texParameteri', 'samplerParameteri',
          'uniform1i', 'uniform1f', 'uniform1iv', 'uniform4f', 'enableVertexAttribArray', 'vertexAttribPointer',
          'vertexAttribIPointer', 'vertexAttribDivisor', 'drawElementsInstanced', 'bufferData']) {
          const original = WebGL2RenderingContext.prototype[name];
          WebGL2RenderingContext.prototype[name] = function(...args) {
            window.kinGLCalls[name] = (window.kinGLCalls[name] || 0) + 1;
            return Reflect.apply(original, this, args);
          };
        }
      }
      const request = window.requestAnimationFrame.bind(window);
      window.kinFrameSamples = [];
      window.requestAnimationFrame = callback => request(time => {
        const calls = ${values["gl-calls"]} ? {...window.kinGLCalls} : null;
        const started = performance.now();
        try { return callback(time); }
        finally {
          const game = window.Module?.kinSummit;
          if (game) {
            const sample = {frame:game.frames,phase:game.phase,ms:performance.now()-started,fillMs:game.fillMs,uiMs:game.uiMs};
            if (calls) sample.glCalls = Object.fromEntries(Object.entries(window.kinGLCalls).map(([name, n]) => [name, n-(calls[name] || 0)]));
            window.kinFrameSamples.push(sample);
            if (window.kinFrameSamples.length>1200) window.kinFrameSamples.shift();
          }
        }
      });
    })()`,
    });
  }
  await call("Page.navigate", {
    url: `http://127.0.0.1:${server.address().port}/embed.html`,
  });
  {
    await until(
      () =>
        evaluate(`(() => {
    const w = document.querySelector('iframe')?.contentWindow;
    return !!(w?.Module && w.document.getElementById('status').hidden && w.document.getElementById('canvas').getContext('webgl2'));
  })()`),
      "Wasm and WebGL2 initialization",
    );
    await delay(1000);
    const info = await evaluate(`(() => {
    const w = document.querySelector('iframe').contentWindow, c = w.Module.canvas, gl = c.getContext('webgl2');
    const limits = Object.fromEntries(['MAX_TEXTURE_SIZE', 'MAX_RENDERBUFFER_SIZE', 'MAX_VERTEX_ATTRIBS',
      'MAX_UNIFORM_BLOCK_SIZE', 'MAX_UNIFORM_BUFFER_BINDINGS', 'MAX_TEXTURE_IMAGE_UNITS',
      'MAX_VERTEX_TEXTURE_IMAGE_UNITS', 'MAX_COLOR_ATTACHMENTS', 'MAX_DRAW_BUFFERS', 'MAX_SAMPLES']
      .map(name => [name, gl.getParameter(gl[name])]));
    const debug = gl.getExtension('WEBGL_debug_renderer_info');
    return {version: gl.getParameter(gl.VERSION), renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER), error: gl.getError(), limits, isolated: w.crossOriginIsolated,
      width:c.width, height:c.height, status:w.document.getElementById('status').hidden, message:w.document.getElementById('status').textContent};
  })()`);
    assert(info.version.startsWith("WebGL 2.0"));
    assert.equal(info.error, 0);
    assert.equal(
      info.isolated,
      false,
      "default export must work without COOP/COEP",
    );
    assert(
      info.status && info.width > 0 && info.height > 0,
      JSON.stringify({ info, errors }),
    );
    async function screenshot(name) {
      const result = await call("Page.captureScreenshot", { format: "png" });
      await writeFile(
        join(output, name + ".png"),
        Buffer.from(result.data, "base64"),
      );
      return result.data;
    }
    const initial = await screenshot("initial");
    async function key(key, code, virtualKey) {
      await call("Input.dispatchKeyEvent", {
        type: "keyDown",
        key,
        code,
        windowsVirtualKeyCode: virtualKey,
      });
      await delay(80);
      await call("Input.dispatchKeyEvent", {
        type: "keyUp",
        key,
        code,
        windowsVirtualKeyCode: virtualKey,
      });
      await delay(200);
    }
    let fixture, profile;
    if (values.mode === "3d") {
      fixture = await until(
        () =>
          evaluate(
            `document.querySelector('iframe').contentWindow.Module.kin3dFixture`,
          ),
        "3D pixel oracle",
      );
      assert(
        fixture.passed &&
          fixture.materials &&
          fixture.skinning &&
          fixture.sky &&
          fixture.fog &&
          fixture.shadows &&
          fixture.frames > 1 &&
          fixture.draws === 2 &&
          fixture.instances === 3 &&
          fixture.culled === 3,
        JSON.stringify(fixture),
      );
      await key("ArrowRight", "ArrowRight", 39);
      fixture = await evaluate(
        `document.querySelector('iframe').contentWindow.Module.kin3dFixture`,
      );
      assert(fixture.input > 0, "3D fixture must receive keyboard input");
      assert.notEqual(
        await screenshot("turned"),
        initial,
        "Keyboard must turn the lit 3D preview",
      );
    } else if (values.mode === "hex") {
      await key("Tab", "Tab", 9);
      assert.notEqual(
        await screenshot("orientation"),
        initial,
        "Tab must change the hex layout",
      );
      assert.equal(
        await evaluate(
          `document.querySelector('iframe').contentDocument.activeElement.id`,
        ),
        "canvas",
        "Tab must keep game focus",
      );
      await key("r", "KeyR", 82);
      await call("Input.dispatchMouseEvent", {
        type: "mouseMoved",
        x: 650,
        y: 350,
      });
      await delay(100);
      const beforeClick = await screenshot("before-click");
      await call("Input.dispatchMouseEvent", {
        type: "mousePressed",
        x: 650,
        y: 350,
        button: "left",
        clickCount: 1,
      });
      await delay(100);
      await call("Input.dispatchMouseEvent", {
        type: "mouseReleased",
        x: 650,
        y: 350,
        button: "left",
        clickCount: 1,
      });
      await delay(300);
      assert.notEqual(
        await screenshot("picked"),
        beforeClick,
        "Click must update the selected path",
      );
      const beforeMove = await screenshot("before-move");
      await call("Input.dispatchMouseEvent", {
        type: "mousePressed",
        x: 650,
        y: 350,
        button: "right",
        clickCount: 1,
      });
      await delay(100);
      await call("Input.dispatchMouseEvent", {
        type: "mouseReleased",
        x: 650,
        y: 350,
        button: "right",
        clickCount: 1,
      });
      await delay(300);
      assert.notEqual(
        await screenshot("moved"),
        beforeMove,
        "Right click must move the unit",
      );
    }
    const fullscreenButton = await evaluate(`(() => {
    const frame = document.querySelector('iframe'), a = frame.getBoundingClientRect();
    const b = frame.contentDocument.getElementById('fullscreen').getBoundingClientRect();
    return {x:a.x+b.x+b.width/2, y:a.y+b.y+b.height/2};
  })()`);
    await call("Input.dispatchMouseEvent", {
      type: "mousePressed",
      ...fullscreenButton,
      button: "left",
      clickCount: 1,
    });
    await call("Input.dispatchMouseEvent", {
      type: "mouseReleased",
      ...fullscreenButton,
      button: "left",
      clickCount: 1,
    });
    await until(
      () => evaluate("!!document.fullscreenElement"),
      "fullscreen entry",
    );
    await screenshot("fullscreen");
    await evaluate("document.exitFullscreen()");
    await until(
      () => evaluate("!document.fullscreenElement"),
      "fullscreen exit",
    );
    await evaluate(
      `document.querySelector('iframe').style.cssText='width:480px;height:320px'`,
    );
    await until(
      () =>
        evaluate(
          `document.querySelector('iframe').contentWindow.Module.canvas.width === 480 && document.querySelector('iframe').contentWindow.Module.canvas.height === 320`,
        ),
      "canvas resize",
    );
    await screenshot("resized");
    const error = await evaluate(
      `document.querySelector('iframe').contentWindow.Module.canvas.getContext('webgl2').getError()`,
    );
    assert.equal(error, 0);
    if (values.mode === "3d") {
      fixture = await evaluate(
        `document.querySelector('iframe').contentWindow.Module.kin3dFixture`,
      );
      assert(
        fixture.resizes >= 2,
        "3D view targets must resize with the canvas",
      );
      await key("Escape", "Escape", 27);
      await until(
        () =>
          evaluate(
            `document.querySelector('iframe').contentWindow.Module.kin3dFixture.stopped`,
          ),
        "owned scene shutdown",
      );
      const frames = await evaluate(
        `document.querySelector('iframe').contentWindow.Module.kin3dFixture.frames`,
      );
      await delay(300);
      assert.equal(
        await evaluate(
          `document.querySelector('iframe').contentWindow.Module.kin3dFixture.frames`,
        ),
        frames,
        "Animation callback must stop after quit",
      );
      fixture = await evaluate(
        `document.querySelector('iframe').contentWindow.Module.kin3dFixture`,
      );
    }
    assert.deepEqual(errors, [], JSON.stringify(errors));
    await writeFile(
      join(output, "result.json"),
      JSON.stringify(
        {
          ...info,
          mode: values.mode,
          fixture,
          profile,
          keyboard: true,
          picking: values.mode === "hex",
          movement: values.mode !== "3d",
          storageFallback: values["deny-storage"],
          fullscreen: true,
          resize: true,
          errors,
        },
        null,
        2,
      ),
    );
    console.log(
      JSON.stringify({
        ...info,
        mode: values.mode,
        fixture,
        profile,
        keyboard: true,
        fullscreen: true,
        resize: true,
        output,
      }),
    );
  }
} catch (error) {
  console.error(error);
  if (errors.length) console.error(JSON.stringify(errors));
  throw error;
} finally {
  await writeFile(join(output, "chrome.log"), chromeLog);
  await writeFile(join(output, "console.log"), consoleLines.join("\n"));
  socket?.close();
  chrome.kill("SIGTERM");
  await Promise.race([
    new Promise((resolve) =>
      chrome.exitCode !== null || chrome.signalCode
        ? resolve()
        : chrome.once("exit", resolve),
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
