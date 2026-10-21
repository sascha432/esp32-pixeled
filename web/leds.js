// The LED pages of the web UI (docs/ui/web/web-app.md section 4): control and settings, and the
// layout with its preview. Loaded by app.js on the first visit of one of them; uses its helpers
// (api, render, card, facts, esc, busy, toast, refresh).
"use strict";

window.pxLeds = (() => {
  const watts = (mw) => `${(mw / 1000).toFixed(1)} W`;

  // ----------------------------------------------------------------------------- the LEDs page

  async function leds() {
    render(`<h1>LEDs</h1><div id="leds-control">Loading ...</div><div id="leds-motion"></div><div id="leds-config"></div>`);
    let s = await api("/api/leds");

    const statusFacts = () => facts([
      ["State", s.error ? `<span class="text-danger">${esc(s.error)}</span>`
        : s.supply_on ? `on, ${s.fps} fps` : "off"],
      ["Power", s.supply_on ? `${watts(s.power_mw)}${s.limited ? ' <span class="badge text-bg-warning">limited</span>' : ""}` : "-"],
      ["LEDs", s.leds ? `${s.leds} on ${s.lanes} lanes, canvas ${s.width} x ${s.height}` : "none"],
    ]);
    const effectOf = (slug) => s.effects.find((e) => e.slug === slug) || {};

    $("#leds-control").innerHTML = card("Light", `
      <div class="form-check form-switch mb-3">
        <input class="form-check-input" type="checkbox" role="switch" id="leds-on" ${s.on ? "checked" : ""}>
        <label class="form-check-label" for="leds-on">On</label>
      </div>
      <label class="form-label" for="leds-brightness">Brightness <span id="leds-brightness-value" class="text-body-secondary">${s.brightness}</span></label>
      <input id="leds-brightness" type="range" class="form-range mb-3" min="1" max="255" value="${s.brightness}">
      <label class="form-label" for="leds-effect">Animation</label>
      <select id="leds-effect" class="form-select mb-3">
        ${s.effects.map((e) => `<option value="${esc(e.slug)}" ${e.slug === s.effect ? "selected" : ""}>${esc(e.name)}</option>`).join("")}
      </select>
      <div id="leds-color-row" class="mb-3">
        <label class="form-label" for="leds-color">Color</label>
        <input id="leds-color" type="color" class="form-control form-control-color" value="${esc(s.color)}">
      </div>
      <label class="form-label" for="leds-speed">Speed <span id="leds-speed-value" class="text-body-secondary">${s.speed}</span></label>
      <input id="leds-speed" type="range" class="form-range mb-3" min="0" max="255" value="${s.speed}">
      <div id="leds-status">${statusFacts()}</div>`);

    const showColor = () => ($("#leds-color-row").hidden = !effectOf($("#leds-effect").value).color);
    showColor();
    const send = (form) => api("/api/leds", form).then((answer) => {
      s = answer;
      $("#leds-status").innerHTML = statusFacts();
      if ($("#motion-status")) $("#motion-status").innerHTML = motionFacts();
    }).catch((e) => toast(e.message, "danger"));
    // a slider sends while it moves, at most every 150 ms, and once more where it stops
    const slider = (id, key) => {
      const input = $(id);
      let timer = null;
      input.oninput = () => {
        $(`${id}-value`).textContent = input.value;
        if (!timer) timer = setTimeout(() => { timer = null; send({ [key]: input.value }); }, 150);
      };
      input.onchange = () => send({ [key]: input.value });
    };
    slider("#leds-brightness", "brightness");
    slider("#leds-speed", "speed");
    $("#leds-on").onchange = (e) => send({ on: e.target.checked ? "1" : "0" });
    $("#leds-effect").onchange = (e) => { showColor(); send({ effect: e.target.value }); };
    $("#leds-color").oninput = (e) => send({ color: e.target.value });

    // motion auto off: only with a sensor
    const motionFacts = () => {
      const m = s.motion;
      return facts([
        ["Sensor", m.detected ? (m.off_in_s ? `motion, holds for ${duration(m.off_in_s)}` : "motion") : "clear"],
        ["Auto off", !m.auto_off ? "off" : m.off_by_motion ? "switched off by motion, on with the next motion"
          : m.off_in_s ? `off in ${duration(m.off_in_s)} without motion` : s.on ? "waiting for motion" : "-"],
      ]);
    };
    if (s.motion.sensor) {
      $("#leds-motion").innerHTML = card("Motion", `
        <div class="form-check form-switch mb-3">
          <input class="form-check-input" type="checkbox" role="switch" id="motion-auto" ${s.motion.auto_off ? "checked" : ""}>
          <label class="form-check-label" for="motion-auto">Off when nobody moves</label>
        </div>
        <label class="form-label" for="motion-minutes">Minutes without motion</label>
        <input id="motion-minutes" type="number" class="form-control mb-1" min="1" max="240" value="${s.motion.minutes}" style="max-width:8em">
        <div class="form-text mb-3">Every motion the sensor sees starts these minutes again; when they run out the LEDs go off
          (and the motion sensor in Home Assistant clears). The next motion switches the LEDs on again, but only when motion
          switched them off: off by hand stays off.</div>
        <div id="motion-status">${motionFacts()}</div>`);
      $("#motion-auto").onchange = (e) => send({ auto_off: e.target.checked ? "1" : "0" });
      $("#motion-minutes").onchange = (e) => send({ auto_off_minutes: e.target.value });
    }
    const c = s.config;
    $("#leds-config").innerHTML = card("Power and timing", `
      <form id="leds-config-form">
        <div class="row g-3">
          <div class="col-sm-6"><label class="form-label" for="cfg-limit">Power limit (W, 0: none)</label>
            <input id="cfg-limit" type="number" class="form-control" min="0" step="0.1" value="${c.power_limit_mw / 1000}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-fps">Frames per second</label>
            <input id="cfg-fps" type="number" class="form-control" min="1" max="200" value="${c.fps}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-fade">Fade (ms)</label>
            <input id="cfg-fade" type="number" class="form-control" min="0" max="10000" value="${c.fade_ms}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-settle">Supply settle time (ms)</label>
            <input id="cfg-settle" type="number" class="form-control" min="0" max="2000" value="${c.settle_ms}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-channel">One color of one LED at full (mW)</label>
            <input id="cfg-channel" type="number" class="form-control" min="1" max="1000" value="${c.channel_mw}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-idle">One LED while dark (mW)</label>
            <input id="cfg-idle" type="number" class="form-control" min="0" max="100" value="${c.idle_mw}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-driver">Output</label>
            <select id="cfg-driver" class="form-select" ${c.parallel_supported ? "" : "disabled"}>
              <option value="rmt" ${c.driver === "rmt" ? "selected" : ""}>RMT (one channel per lane)</option>
              <option value="parallel" ${c.driver === "parallel" ? "selected" : ""}>Parallel (all lanes by DMA)</option>
            </select></div>
          <div class="col-sm-6 d-flex align-items-end"><div class="form-check form-switch">
            <input class="form-check-input" type="checkbox" role="switch" id="cfg-dither" ${c.dither ? "checked" : ""}>
            <label class="form-check-label" for="cfg-dither">Dithering (finer steps for dark colors)</label></div></div>
        </div>
        <div class="form-text mt-2">The power is an estimate from the last two values; every frame above the limit is
          dimmed. Measured at the supply it can differ: compare and adjust.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>`);
    $("#leds-config-form").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#leds-config-form button"), async () => {
        s = await api("/api/leds/config", {
          power_limit_mw: Math.round(Number($("#cfg-limit").value) * 1000), fps: $("#cfg-fps").value,
          fade_ms: $("#cfg-fade").value, settle_ms: $("#cfg-settle").value,
          channel_mw: $("#cfg-channel").value, idle_mw: $("#cfg-idle").value, driver: $("#cfg-driver").value,
          dither: $("#cfg-dither").checked ? "1" : "0",
        });
        toast("saved", "success");
      });
    };

    refresh(async () => {
      s = await api("/api/leds");
      const box = $("#leds-status");
      if (box) box.innerHTML = statusFacts();
      const motion = $("#motion-status");
      if (motion) motion.innerHTML = motionFacts();
    }, 2000);
  }

  // ---------------------------------------------------------------------------- the layout page

  const corners = [["tl", "top left"], ["tr", "top right"], ["bl", "bottom left"], ["br", "bottom right"]];

  // the canvas position of LED `i` of a panel: the firmware's rule (components/layout) backwards,
  // for the drawing only; the device checks the layout
  function position(p, i) {
    const columns = p.runs === "columns";
    const length = columns ? p.height : p.width;
    const run = Math.floor(i / length);
    let place = i % length;
    if (p.zigzag && run % 2 === 1) place = length - 1 - place;
    const fromSide = columns ? run : place;
    const fromEnd = columns ? place : run;
    const left = p.first === "tl" || p.first === "bl";
    const top = p.first === "tl" || p.first === "tr";
    return [p.x + (left ? fromSide : p.width - 1 - fromSide), p.y + (top ? fromEnd : p.height - 1 - fromEnd)];
  }

  // The preview: the canvas as cells, every panel in the color of its lane, the data path through
  // its LEDs and a dot on its first one. The lane colors are the one place with colors of its own
  // (docs/ui/web/web-design.md section 2).
  function drawPreview(canvas, layout) {
    const ctx = canvas.getContext("2d");
    const width = Math.max(1, layout.width), height = Math.max(1, layout.height);
    const cell = Math.max(4, Math.min(24, Math.floor((canvas.parentElement.clientWidth - 2) / width)));
    canvas.width = width * cell;
    canvas.height = height * cell;
    const style = getComputedStyle(document.body);
    ctx.fillStyle = style.getPropertyValue("--bs-tertiary-bg") || "#eee";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    const center = ([x, y]) => [x * cell + cell / 2, y * cell + cell / 2];
    layout.panels.forEach((p) => {
      const hue = ((p.lane - 1) * 67) % 360;
      ctx.fillStyle = `hsla(${hue}, 70%, 55%, 0.25)`;
      ctx.fillRect(p.x * cell, p.y * cell, p.width * cell, p.height * cell);
      ctx.strokeStyle = `hsl(${hue}, 70%, 45%)`;
      ctx.lineWidth = 2;
      ctx.strokeRect(p.x * cell + 1, p.y * cell + 1, p.width * cell - 2, p.height * cell - 2);
      ctx.lineWidth = Math.max(1, cell / 8);
      ctx.beginPath();
      const count = p.width * p.height;
      for (let i = 0; i < count; i++) {
        const [cx, cy] = center(position(p, i));
        if (i === 0) ctx.moveTo(cx, cy); else ctx.lineTo(cx, cy);
      }
      ctx.stroke();
      const [fx, fy] = center(position(p, 0));
      ctx.fillStyle = `hsl(${hue}, 80%, 40%)`;
      ctx.beginPath();
      ctx.arc(fx, fy, Math.max(3, cell / 3), 0, 2 * Math.PI);
      ctx.fill();
      ctx.fillStyle = style.getPropertyValue("--bs-body-color") || "#000";
      ctx.font = `${Math.max(10, Math.min(16, cell))}px sans-serif`;
      ctx.fillText(`${p.lane}`, p.x * cell + 4, p.y * cell + Math.max(12, Math.min(18, cell + 2)));
    });
  }

  async function layout() {
    render(`<h1>Layout</h1><div class="row g-3"><div class="col-xl-6" id="layout-form">Loading ...</div>
      <div class="col-xl-6"><div id="layout-lanes" class="mb-3"></div><div class="card"><div class="card-body"><h2 class="h6 card-title">Preview</h2>
        <div><canvas id="layout-canvas" class="border rounded"></canvas></div>
        <p class="small text-body-secondary mt-2 mb-0">Each panel in the color of its lane, the line follows the data from the
          dot (the first LED).</p></div></div></div></div>`);
    const stored = await api("/api/leds/layout");
    const l = { width: stored.width || 16, height: stored.height || 16, panels: stored.panels };
    const lanes = Math.max(1, stored.available_lanes);

    const panelRow = (p, i) => `
      <div class="border rounded p-2 mb-2" data-index="${i}">
        <div class="d-flex justify-content-between align-items-center mb-2">
          <span class="fw-semibold">Panel ${i + 1}</span>
          <button class="btn btn-sm btn-outline-danger" type="button" data-remove="${i}">Remove</button>
        </div>
        <div class="row g-2">
          <div class="col-4 col-md-2"><label class="form-label small">Lane</label>
            <select class="form-select form-select-sm" data-field="lane">
              ${Array.from({ length: lanes }, (_, n) => `<option ${p.lane === n + 1 ? "selected" : ""}>${n + 1}</option>`).join("")}
            </select></div>
          <div class="col-4 col-md-2"><label class="form-label small">X</label>
            <input class="form-control form-control-sm" type="number" min="0" data-field="x" value="${p.x}"></div>
          <div class="col-4 col-md-2"><label class="form-label small">Y</label>
            <input class="form-control form-control-sm" type="number" min="0" data-field="y" value="${p.y}"></div>
          <div class="col-4 col-md-2"><label class="form-label small">Width</label>
            <input class="form-control form-control-sm" type="number" min="1" data-field="width" value="${p.width}"></div>
          <div class="col-4 col-md-2"><label class="form-label small">Height</label>
            <input class="form-control form-control-sm" type="number" min="1" data-field="height" value="${p.height}"></div>
          <div class="col-6 col-md-4"><label class="form-label small">First LED</label>
            <select class="form-select form-select-sm" data-field="first">
              ${corners.map(([v, t]) => `<option value="${v}" ${p.first === v ? "selected" : ""}>${t}</option>`).join("")}
            </select></div>
          <div class="col-6 col-md-4"><label class="form-label small">The LEDs run along</label>
            <select class="form-select form-select-sm" data-field="runs">
              <option value="rows" ${p.runs === "rows" ? "selected" : ""}>rows</option>
              <option value="columns" ${p.runs === "columns" ? "selected" : ""}>columns</option>
            </select></div>
          <div class="col-12 col-md-4 d-flex align-items-end"><div class="form-check">
            <input class="form-check-input" type="checkbox" data-field="zigzag" id="zz${i}" ${p.zigzag ? "checked" : ""}>
            <label class="form-check-label small" for="zz${i}">Zigzag (every other run back)</label></div></div>
        </div>
      </div>`;

    const draw = () => {
      $("#layout-form").innerHTML = card("Canvas and panels", `
        <div class="row g-2 mb-3">
          <div class="col-6"><label class="form-label" for="layout-width">Canvas width</label>
            <input id="layout-width" class="form-control" type="number" min="1" value="${l.width}"></div>
          <div class="col-6"><label class="form-label" for="layout-height">Canvas height</label>
            <input id="layout-height" class="form-control" type="number" min="1" value="${l.height}"></div>
        </div>
        <div id="layout-panels">${l.panels.map(panelRow).join("")}</div>
        <div class="d-flex flex-wrap gap-2 justify-content-end mt-3">
          <button id="layout-add" class="btn btn-outline-secondary" type="button">Add panel</button>
          <button id="layout-edges" class="btn btn-outline-secondary" type="button">Show the edges</button>
          <button id="layout-save" class="btn btn-primary" type="button">Save</button>
        </div>
        <div class="form-text mt-2">A strip is a panel of one row. Panels on the same lane follow each other in this order.
          "Show the edges" draws the canvas border: top red, right green, bottom blue, left yellow, top left white.</div>`);
      wire();
      drawPreview($("#layout-canvas"), l);
    };

    // the inputs into `l`, then the preview
    const read = () => {
      l.width = Number($("#layout-width").value) || 1;
      l.height = Number($("#layout-height").value) || 1;
      document.querySelectorAll("#layout-panels [data-index]").forEach((row) => {
        const p = l.panels[Number(row.dataset.index)];
        row.querySelectorAll("[data-field]").forEach((input) => {
          const field = input.dataset.field;
          p[field] = input.type === "checkbox" ? input.checked
            : ["first", "runs"].includes(field) ? input.value : Number(input.value);
        });
      });
      drawPreview($("#layout-canvas"), l);
    };

    const wire = () => {
      $("#layout-form").oninput = read;
      $("#layout-form").onchange = read;
      document.querySelectorAll("[data-remove]").forEach((b) => (b.onclick = () => {
        read();
        l.panels.splice(Number(b.dataset.remove), 1);
        draw();
      }));
      $("#layout-add").onclick = () => {
        read();
        const last = l.panels[l.panels.length - 1];
        l.panels.push(last ? { ...last, lane: Math.min(lanes, last.lane + 1), x: last.x + last.width }
          : { lane: 1, x: 0, y: 0, width: l.width, height: l.height, first: "tl", runs: "rows", zigzag: true });
        l.width = Math.max(l.width, ...l.panels.map((p) => p.x + p.width));
        draw();
      };
      $("#layout-save").onclick = (e) => busy(e.target, async () => {
        read();
        const panels = l.panels.map((p) =>
          [p.lane, p.x, p.y, p.width, p.height, p.first, p.runs, p.zigzag ? "zigzag" : "straight"].join(",")).join(";");
        await api("/api/leds/layout", { width: l.width, height: l.height, panels });
        toast("layout saved", "success");
      });
      $("#layout-edges").onclick = (e) => busy(e.target, async () => {
        await api("/api/leds", { effect: "edges", on: "1" });
        toast("edges shown: compare them with the preview", "success");
      });
    };

    draw();
    await drawLanes();
  }

  // Lanes: what each lane does to its colors, for LEDs of another kind on one lane. Every change
  // is saved at once.
  async function drawLanes() {
    const data = await api("/api/leds/lanes");
    const orders = ["grb", "rgb", "brg", "rbg", "gbr", "bgr"];
    $("#layout-lanes").innerHTML = card("Lanes", `
      <div class="table-responsive"><table class="table table-sm align-middle mb-2">
        <thead><tr><th>Lane</th><th>Order</th><th>Level</th><th>Red</th><th>Green</th><th>Blue</th><th>Dither</th></tr></thead>
        <tbody>${data.lanes.map((l) => `<tr data-lane="${l.lane}">
          <td>${l.lane}</td>
          <td><select class="form-select form-select-sm" data-key="order">${orders.map((o) =>
            `<option ${o === l.order ? "selected" : ""}>${o}</option>`).join("")}</select></td>
          ${["level", "red", "green", "blue"].map((k) =>
            `<td><input class="form-control form-control-sm" type="number" min="0" max="255" data-key="${k}" value="${l[k]}" style="width:5.5em"></td>`).join("")}
          <td><input class="form-check-input" type="checkbox" data-key="dither" ${l.dither ? "checked" : ""}></td>
        </tr>`).join("")}</tbody></table></div>
      <div class="form-text">For LEDs of another kind on a lane: the color order of their chip, their brightness against
        the others (255: the same), the color balance, and dithering (off for LEDs that flicker in dark colors).</div>`);
    document.querySelectorAll("#layout-lanes [data-key]").forEach((input) => {
      input.onchange = () => {
        const lane = input.closest("[data-lane]").dataset.lane;
        const value = input.type === "checkbox" ? (input.checked ? "1" : "0") : input.value;
        api("/api/leds/lanes", { lane, [input.dataset.key]: value })
          .then(() => toast("saved", "success"), (e) => toast(e.message, "danger"));
      };
    });
  }

  return { leds, layout };
})();
