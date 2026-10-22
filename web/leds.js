// The LED pages of the web UI (docs/ui/web/web-app.md section 4): control and settings, and the
// layout with its preview. Loaded by app.js on the first visit of one of them; uses its helpers
// (api, render, card, facts, esc, busy, toast, refresh).
"use strict";

window.pxLeds = (() => {
  const watts = (mw) => `${(mw / 1000).toFixed(1)} W`;

  // ----------------------------------------------------------------------------- the LEDs page

  async function leds() {
    render(`<h1>LEDs</h1><div id="leds-live"></div><div id="leds-control">Loading ...</div><div id="leds-presets"></div><div id="leds-overlay"></div><div id="leds-motion"></div><div id="leds-buttons"></div><div id="leds-audio"></div><div id="leds-config"></div>`);
    let s = await api("/api/leds");

    const statusFacts = () => facts([
      ["State", s.error ? `<span class="text-danger">${esc(s.error)}</span>`
        : s.supply_on ? `on, ${s.fps} fps` : "off"],
      ["Power", s.supply_on ? `${watts(s.power_mw)}${s.limited ? ' <span class="badge text-bg-warning">limited</span>' : ""}` : "-"],
      ["LEDs", s.leds ? `${s.leds} on ${s.lanes} lanes, canvas ${s.width} x ${s.height}` : "none"],
      ...(s.audio.source ? [["Sound", s.audio.hears ? "hears sound" : "silence"]] : []),
    ]);
    const effectOf = (slug) => s.effects.find((e) => e.slug === slug) || {};
    // an animation that needs sound only with a source (the current one stays listed)
    const listed = (e) => e.reactive !== "required" || s.audio.source || e.slug === s.effect;
    // the sound comes with the live socket; the card is drawn further down
    let mic = null;
    const live = preview((frame) => mic?.live(frame));

    $("#leds-control").innerHTML = card("Light", `
      <div class="form-check form-switch mb-3">
        <input class="form-check-input" type="checkbox" role="switch" id="leds-on" ${s.on ? "checked" : ""}>
        <label class="form-check-label" for="leds-on">On</label>
      </div>
      <label class="form-label" for="leds-brightness">Brightness <span id="leds-brightness-value" class="text-body-secondary">${s.brightness}</span></label>
      <input id="leds-brightness" type="range" class="form-range mb-3" min="1" max="255" value="${s.brightness}">
      <label class="form-label" for="leds-effect">Animation</label>
      <select id="leds-effect" class="form-select mb-3">
        ${s.effects.filter(listed).map((e) => `<option value="${esc(e.slug)}" ${e.slug === s.effect ? "selected" : ""}>${esc(e.name)}</option>`).join("")}
      </select>
      <div id="leds-palette-row" class="mb-3">
        <label class="form-label" for="leds-palette">Palette</label>
        <select id="leds-palette" class="form-select">
          ${s.palettes.map((p) => `<option value="${esc(p.slug)}">${esc(p.name)}</option>`).join("")}
        </select>
      </div>
      <div id="leds-color-row" class="mb-3">
        <label class="form-label" for="leds-color">Color</label>
        <input id="leds-color" type="color" class="form-control form-control-color" value="${esc(s.color)}">
      </div>
      <label class="form-label" for="leds-speed">Speed <span id="leds-speed-value" class="text-body-secondary">${s.speed}</span></label>
      <input id="leds-speed" type="range" class="form-range mb-3" min="0" max="255" value="${s.speed}">
      <div class="row g-3 mb-3 align-items-end">
        <div class="col-auto"><label class="form-label" for="leds-rotation">Rotation</label>
          <select id="leds-rotation" class="form-select">
            ${[0, 90, 180, 270].map((r) => `<option value="${r}">${r}°</option>`).join("")}
          </select></div>
        <div class="col-auto" id="leds-reactive-row"><div class="form-check form-switch mb-2">
          <input class="form-check-input" type="checkbox" role="switch" id="leds-reactive">
          <label class="form-check-label" for="leds-reactive">Reacts to sound</label></div></div>
      </div>
      <div id="leds-strength-row">
        <label class="form-label" for="leds-strength">Strength <span id="leds-strength-value" class="text-body-secondary">${s.strength}</span></label>
        <input id="leds-strength" type="range" class="form-range mb-1" min="0" max="255" value="${s.strength}">
        <div class="form-text mb-2">How strongly the animation follows the sound: 128 as built, 255 twice as much, 0 not at all.</div>
        <div class="row g-3">
          <div class="col-sm-6"><label class="form-label" for="leds-attack">Attack <span id="leds-attack-value" class="text-body-secondary">${s.attack}</span> ms</label>
            <input id="leds-attack" type="range" class="form-range" min="0" max="2000" step="10" value="${s.attack}"></div>
          <div class="col-sm-6"><label class="form-label" for="leds-release">Release <span id="leds-release-value" class="text-body-secondary">${s.release}</span> ms</label>
            <input id="leds-release" type="range" class="form-range" min="0" max="5000" step="10" value="${s.release}"></div>
        </div>
        <div class="form-text mb-2">How fast it follows a louder sound (attack) and lets go when it gets quieter (release):
          20 and 250 ms as built; a long release lets the bars and the light fall slowly.</div>
      </div>
      <div class="form-text mb-3">Rotation, palette and the sound settings are kept for each animation.</div>
      <div id="leds-status">${statusFacts()}</div>`);

    // the color for an animation that shows it, or with the palette made from it
    const showColor = () => {
      const effect = effectOf($("#leds-effect").value);
      $("#leds-palette-row").hidden = !effect.palette;
      $("#leds-color-row").hidden = !effect.color && !(effect.palette && s.palette === "color");
    };
    showColor();
    // the current animation's rotation and sound switch; the switch only for one that can react,
    // with an audio source
    const showStyle = () => {
      $("#leds-rotation").value = String(s.rotation);
      $("#leds-palette").value = s.palette;
      showColor();
      $("#leds-reactive").checked = s.reactive;
      const reactive = effectOf(s.effect).reactive;
      const canReact = s.audio.source && reactive === "optional";
      $("#leds-reactive-row").hidden = !canReact;
      // the strength, attack and release with the switch on (always for one that needs sound); not
      // moved while a slider is being dragged
      $("#leds-strength-row").hidden = !s.audio.source || !((canReact && s.reactive) || reactive === "required");
      for (const key of ["strength", "attack", "release"]) {
        if (document.activeElement !== $(`#leds-${key}`)) {
          $(`#leds-${key}`).value = String(s[key]);
          $(`#leds-${key}-value`).textContent = s[key];
        }
      }
    };
    showStyle();
    // the presets card follows the light (the preset it matches); drawn further down
    let presets = { update: async () => {} };
    const send = (form) => api("/api/leds", form).then((answer) => {
      s = answer;
      showStyle();
      $("#leds-status").innerHTML = statusFacts();
      if ($("#motion-status")) $("#motion-status").innerHTML = motionFacts();
      presets.update();
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
    slider("#leds-strength", "strength");
    slider("#leds-attack", "attack");
    slider("#leds-release", "release");
    $("#leds-on").onchange = (e) => send({ on: e.target.checked ? "1" : "0" });
    $("#leds-effect").onchange = (e) => { showColor(); send({ effect: e.target.value }); };
    overlayCard(s, (answer) => { s = answer; });
    $("#leds-color").oninput = (e) => send({ color: e.target.value });
    $("#leds-rotation").onchange = (e) => send({ rotation: e.target.value });
    $("#leds-palette").onchange = (e) => send({ palette: e.target.value });
    // a preset sets the whole light: the card's controls follow
    presets = await presetCard((answer) => {
      s = answer;
      $("#leds-on").checked = s.on;
      for (const key of ["brightness", "speed"]) {
        $(`#leds-${key}`).value = String(s[key]);
        $(`#leds-${key}-value`).textContent = s[key];
      }
      $("#leds-effect").value = s.effect;
      $("#leds-color").value = s.color;
      showStyle();
    });
    await buttonCard();
    $("#leds-reactive").onchange = (e) => send({ reactive: e.target.checked ? "1" : "0" });

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
          <div class="col-sm-6"><label class="form-label" for="cfg-crossfade">Crossfade between animations (ms, 0: none)</label>
            <input id="cfg-crossfade" type="number" class="form-control" min="0" max="10000" value="${c.crossfade_ms}"></div>
          <div class="col-sm-6"><label class="form-label" for="cfg-power-on">At power on</label>
            <select id="cfg-power-on" class="form-select">
              <option value="restore" ${c.power_on === "restore" ? "selected" : ""}>As before (also "off by motion")</option>
              <option value="off" ${c.power_on === "off" ? "selected" : ""}>Off</option>
              <option value="on" ${c.power_on === "on" ? "selected" : ""}>On</option>
            </select></div>
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
          crossfade_ms: $("#cfg-crossfade").value, power_on: $("#cfg-power-on").value,
          channel_mw: $("#cfg-channel").value, idle_mw: $("#cfg-idle").value, driver: $("#cfg-driver").value,
          dither: $("#cfg-dither").checked ? "1" : "0",
        });
        toast("saved", "success");
      });
    };

    // the sound input: the microphone's settings, the UDP inputs and a live readout
    mic = await soundInput();

    // every 2 s the LED status and the sound's slower facts (the bands come with the live socket);
    // the live preview stops with the page (the refresh runs no more)
    let tick = 0;
    refresh(async () => {
      live.alive();
      if (tick++ % 4 !== 0) return;
      if (mic) await mic.update();
      s = await api("/api/leds");
      await presets.update();
      const box = $("#leds-status");
      if (box) box.innerHTML = statusFacts();
      const motion = $("#motion-status");
      if (motion) motion.innerHTML = motionFacts();
    }, 500);
  }

  // The card "Sound input": the analysis as it runs (32 bars, the levels), the microphone's settings
  // (with one) and the UDP inputs. Returns { update } for the page's refresh (every 2 s) and
  // { live } for the frames of the live socket (20 a second), null without the API.
  async function soundInput() {
    let a;
    try {
      a = await api("/api/audio");
    } catch (e) {
      return null;
    }
    const c = a.settings;
    const sources = { microphone: "microphone", warls: "WARLS (UDP)", wled: "WLED audio sync" };
    const db = (v) => `${Number(v).toFixed(1)} dB`;
    // the bars are drawn once and then only change their height
    const bars = `
      <div id="mic-bars" class="d-flex align-items-end gap-1 mb-2" style="height:64px" aria-label="the 32 bands, bass on the left">
        ${a.bands.map(() => '<div class="flex-fill rounded-top bg-secondary" style="height:2px"></div>').join("")}
      </div>`;
    const drawBars = () => {
      const box = $("#mic-bars");
      if (!box) return;
      [...box.children].forEach((bar, n) => {
        bar.style.height = `${Math.max(2, Math.round((a.bands[n] || 0) / 255 * 64))}px`;
        bar.className = `flex-fill rounded-top ${a.active ? "bg-primary" : "bg-secondary"}`;
      });
    };
    let shown = "";
    const drawFacts = () => {
      const html = readout();
      const box = $("#mic-facts");
      if (box && html !== shown) box.innerHTML = shown = html;
    };
    const readout = () => facts([
        ["From", sources[a.source] || "nothing"],
        ["Input", a.active ? `sound${a.beat ? ' <span class="badge text-bg-warning">beat</span>' : ""}` : "silence"],
        ...(a.source === "microphone" ? [["Loudest band", db(a.loudest_db)], ["Reference", db(a.reference_db)]] : []),
        ...(a.source === "warls" || a.source === "wled" ? [["Packets", a.packets]] : []),
        ["Loudness", `${a.loudness} of 255`],
      ]);
    const i = a.inputs;
    $("#leds-audio").innerHTML = card("Sound input", `
      <div id="mic-readout">${bars}<div id="mic-facts"></div></div>
      <form id="udp-form" class="mt-3">
        <div class="row g-3">
          <div class="col-sm-6 d-flex align-items-end"><div class="form-check form-switch">
            <input class="form-check-input" type="checkbox" role="switch" id="udp-warls" ${i.warls ? "checked" : ""}>
            <label class="form-check-label" for="udp-warls">WARLS spectrum over UDP</label></div></div>
          <div class="col-sm-6"><label class="form-label" for="udp-port">WARLS port</label>
            <input id="udp-port" type="number" class="form-control" min="1" max="65535" value="${i.warls_port}"></div>
          <div class="col-sm-6"><div class="form-check form-switch">
            <input class="form-check-input" type="checkbox" role="switch" id="udp-wled" ${i.wled_sync ? "checked" : ""}>
            <label class="form-check-label" for="udp-wled">WLED audio sync (multicast, port 11988)</label></div></div>
        </div>
        <div class="form-text mt-2">Another device sends what it hears: a PC (tools/audio_send.py, the kfc_fw audio
          analyzer), a WLED sound reactive device or LedFx. While its packets come they are used instead of the
          microphone.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>
      <form id="mic-form" class="mt-3" ${a.microphone ? "" : "hidden"}>
        <h3 class="h6">Microphone</h3>
        <div class="row g-3">
          <div class="col-sm-6"><label class="form-label" for="mic-gain">Gain (dB, -20 to 40)</label>
            <input id="mic-gain" type="number" class="form-control" min="-20" max="40" value="${c.gain_db}"></div>
          <div class="col-sm-6 d-flex align-items-end"><div class="form-check form-switch">
            <input class="form-check-input" type="checkbox" role="switch" id="mic-agc" ${c.agc ? "checked" : ""}>
            <label class="form-check-label" for="mic-agc">Automatic gain</label></div></div>
          <div class="col-sm-6"><label class="form-label" for="mic-silence">Silence below (dB, -90 to -20)</label>
            <input id="mic-silence" type="number" class="form-control" min="-90" max="-20" value="${c.silence_db}"></div>
          <div class="col-sm-6"><label class="form-label" for="mic-range">Range (dB, 10 to 80)</label>
            <input id="mic-range" type="number" class="form-control" min="10" max="80" value="${c.range_db}"></div>
          <div class="col-sm-6"><label class="form-label" for="mic-beat">Beat when the bass is (x, 1.2 to 3.0)</label>
            <input id="mic-beat" type="number" class="form-control" min="1.2" max="3" step="0.1" value="${(c.beat_ratio_tenths / 10).toFixed(1)}"></div>
        </div>
        <div class="form-text mt-2">The loudest band must be above the silence level, or the input is silence: set it a
          little above what a quiet room shows. With automatic gain the bars follow the music's own loudness and the gain
          only moves the silence point; without it the gain decides how full the bars get (0 dB at the top). The range is
          what the bars show below the top: smaller is livelier.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>`);
    $("#udp-form").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#udp-form button"), async () => {
        a = await api("/api/audio", {
          warls: $("#udp-warls").checked ? "1" : "0", warls_port: $("#udp-port").value,
          wled_sync: $("#udp-wled").checked ? "1" : "0",
        });
        toast("saved: the LEDs page shows the sound settings once there is a source", "success");
      });
    };
    $("#mic-form").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#mic-form button"), async () => {
        a = await api("/api/audio", {
          gain_db: $("#mic-gain").value, agc: $("#mic-agc").checked ? "1" : "0",
          silence_db: $("#mic-silence").value, range_db: $("#mic-range").value,
          beat_ratio_tenths: Math.round(Number($("#mic-beat").value) * 10),
        });
        toast("saved", "success");
      });
    };
    drawBars();
    drawFacts();
    // without frames from the socket (more than 4 open) the poll draws the bars as well
    let heard = 0;
    return {
      update: async () => {
        const live = Date.now() - heard < 1000 ? { active: a.active, beat: a.beat, loudness: a.loudness, bands: a.bands } : null;
        a = { ...(await api("/api/audio")), ...live };
        if (!live) drawBars();
        drawFacts();
      },
      live: (frame) => {
        heard = Date.now();
        a = { ...a, ...frame };
        drawBars();
        drawFacts();
      },
    };
  }

  // The card "Presets": the saved lights, shown with a click, and the light as it is now saved
  // under a name. `applied` takes the LED state after a preset was shown. Returns { update } for
  // after a change of the light (the preset it matches).
  async function presetCard(applied) {
    let p = await api("/api/presets");
    const draw = () => {
      const rows = p.presets.map((preset, i) => `
        <li class="list-group-item d-flex align-items-center gap-2">
          <span class="rounded border" style="width:1.2em;height:1.2em;background:${esc(preset.color)}"></span>
          <span class="flex-grow-1 text-truncate">${esc(preset.name)}
            ${i === p.current ? '<span class="badge text-bg-primary ms-1">shown</span>' : ""}</span>
          <button class="btn btn-sm btn-outline-primary" data-apply="${i}">Show</button>
          <button class="btn btn-sm btn-outline-danger" data-delete="${i}">Delete</button>
        </li>`).join("");
      $("#leds-presets").innerHTML = card(`Presets (${p.presets.length} of ${p.max})`, `
        <ul class="list-group mb-3">${rows || '<li class="list-group-item text-body-secondary">none yet</li>'}</ul>
        <form id="preset-form" class="d-flex gap-2">
          <input id="preset-name" class="form-control" maxlength="31" placeholder="name" required>
          <button class="btn btn-primary text-nowrap" type="submit">Save the light</button>
        </form>
        <div class="form-text mt-2">A preset keeps the animation with its color, speed, brightness, rotation, palette and
          sound settings. Saving under a name that is there replaces it. Home Assistant lists them as "Preset".</div>`);
      $("#leds-presets ul").onclick = (event) => {
        const b = event.target.closest("button");
        if (!b) return;
        busy(b, async () => {
          if (b.dataset.delete !== undefined) {
            const name = p.presets[Number(b.dataset.delete)].name;
            if (!confirm(`Delete the preset ${name}?`)) return;
            p = await api("/api/presets/delete", { index: b.dataset.delete });
          } else {
            p = await api("/api/presets/apply", { index: b.dataset.apply });
            applied(await api("/api/leds"));
          }
          draw();
        });
      };
      $("#preset-form").onsubmit = (e) => {
        e.preventDefault();
        busy(e.submitter || $("#preset-form button"), async () => {
          p = await api("/api/presets/save", { name: $("#preset-name").value.trim() });
          toast("saved", "success");
          draw();
        });
      };
    };
    draw();
    return {
      update: async () => {
        const before = p.current;
        p = await api("/api/presets");
        if (p.current !== before && !$("#leds-presets").contains(document.activeElement)) draw();
      },
    };
  }

  // The card "Buttons": what each button of the board and the encoder's push do; only with any.
  async function buttonCard() {
    let b;
    try {
      b = await api("/api/input");
    } catch (e) {
      return;
    }
    if (!b.buttons.length && !b.encoder) return;
    const select = (key, value) => `<select class="form-select form-select-sm" data-key="${key}">
      ${b.actions.map((a) => `<option value="${esc(a.slug)}" ${a.slug === value ? "selected" : ""}>${esc(a.name)}</option>`).join("")}
      </select>`;
    const row = (id, title, item) => `<tr data-button="${id}">
      <td class="text-nowrap">${esc(title)}</td>
      <td>${select("click", item.click)}</td><td>${select("double", item.double)}</td><td>${select("hold", item.hold)}</td></tr>`;
    const e = b.encoder;
    const hotspot = b.buttons.find((item) => item.hotspot);
    $("#leds-buttons").innerHTML = card("Buttons", `
      <div class="table-responsive"><table class="table table-sm align-middle mb-2">
        <thead><tr><th>Button</th><th>Click</th><th>Double click</th><th>Hold</th></tr></thead>
        <tbody>${b.buttons.map((item) => row(item.button, `${item.button} (GPIO ${item.gpio})`, item)).join("")}
          ${e && e.button >= 0 ? row("encoder", `Encoder push (GPIO ${e.button})`, e) : ""}</tbody></table></div>
      ${e && e.a >= 0 ? `<p class="small mb-2">Encoder on GPIO ${e.a} and ${e.b}: turning changes the brightness (faster
        when turned fast); after the action "Knob: animations" it changes the animation until it rests for 10 s.</p>` : ""}
      <div class="form-text">A hold acts after 0.6 s; brighter and darker repeat while held; a restart needs 3 s.
        ${hotspot ? `Button ${hotspot.button} held while the device starts (after a reset or at power on) opens the hotspot
        without a password, for a device whose Wi-Fi is gone. ` : ""}A double click delays the click by 0.3 s.</div>`);
    document.querySelectorAll("#leds-buttons [data-key]").forEach((input) => {
      input.onchange = () => {
        const button = input.closest("[data-button]").dataset.button;
        api("/api/input", { button, [input.dataset.key]: input.value })
          .then((answer) => { b = answer; toast("saved", "success"); }, (err) => toast(err.message, "danger"));
      };
    });
  }

  // The card "Overlay": text or the clock over every animation. `update` takes the answer.
  function overlayCard(s, update) {
    const o = s.overlay;
    $("#leds-overlay").innerHTML = card("Overlay", `
      <form id="overlay-form">
        <div class="row g-3">
          <div class="col-sm-6"><label class="form-label" for="overlay-mode">Show</label>
            <select id="overlay-mode" class="form-select">
              ${[["off", "Nothing"], ["text", "Text"], ["clock", "The clock"]].map(([v, t]) =>
                `<option value="${v}" ${o.mode === v ? "selected" : ""}>${t}</option>`).join("")}
            </select></div>
          <div class="col-sm-6"><label class="form-label" for="overlay-color">Color</label>
            <input id="overlay-color" type="color" class="form-control form-control-color" value="${esc(o.color)}"></div>
          <div class="col-12" id="overlay-text-row"><label class="form-label" for="overlay-text">Text</label>
            <input id="overlay-text" class="form-control" maxlength="63" value="${esc(o.text)}"></div>
          <div class="col-sm-6" id="overlay-speed-row"><label class="form-label" for="overlay-speed">Scroll speed
            <span id="overlay-speed-value" class="text-body-secondary">${o.speed}</span></label>
            <input id="overlay-speed" type="range" class="form-range" min="0" max="255" value="${o.speed}"></div>
          <div class="col-sm-6 d-flex align-items-end" id="overlay-24h-row"><div class="form-check form-switch">
            <input class="form-check-input" type="checkbox" role="switch" id="overlay-24h" ${o.clock_24h ? "checked" : ""}>
            <label class="form-check-label" for="overlay-24h">24 hours</label></div></div>
        </div>
        <div class="form-text mt-2">A text wider than the matrix scrolls; the clock needs the time from the network.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>`);
    const show = () => {
      const mode = $("#overlay-mode").value;
      $("#overlay-text-row").hidden = mode !== "text";
      $("#overlay-speed-row").hidden = mode !== "text";
      $("#overlay-24h-row").hidden = mode !== "clock";
    };
    show();
    $("#overlay-mode").onchange = show;
    $("#overlay-speed").oninput = (e) => ($("#overlay-speed-value").textContent = e.target.value);
    $("#overlay-form").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#overlay-form button"), async () => {
        update(await api("/api/leds/overlay", {
          mode: $("#overlay-mode").value, text: $("#overlay-text").value, color: $("#overlay-color").value,
          speed: $("#overlay-speed").value, clock_24h: $("#overlay-24h").checked ? "1" : "0",
        }));
        toast("saved", "success");
      });
    };
  }

  // The card "Live": the canvas as drawn, over a WebSocket (GET /api/leds/live), and the sound for
  // `onAudio`. Shown while the page is open: alive() comes with every refresh; without it for 3 s
  // (another page) the socket closes.
  function preview(onAudio) {
    $("#leds-live").innerHTML = card("Live", `
      <div><canvas id="live-canvas" class="border rounded" style="max-width:100%;image-rendering:pixelated"></canvas></div>
      <div id="live-state" class="small text-body-secondary mt-1">connecting ...</div>`);
    let socket = null;
    let seen = Date.now();
    const open = () => {
      socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/api/leds/live`);
      socket.binaryType = "arraybuffer";
      socket.onmessage = (m) => {
        const data = new Uint8Array(m.data);
        const width = data[0] | (data[1] << 8), height = data[2] | (data[3] << 8);
        const end = 4 + width * height * 3;
        if (width && height) draw(data, width, height);
        // the sound after the pixels: flags (1 active, 2 beat), loudness, 32 bands
        if (data.length >= end + 34) {
          onAudio({ active: !!(data[end] & 1), beat: !!(data[end] & 2), loudness: data[end + 1],
            bands: Array.from(data.subarray(end + 2, end + 34)) });
        }
      };
      socket.onclose = () => {
        const state = $("#live-state");
        if (state) state.textContent = "no preview (more than 4 open, or the connection closed)";
        socket = null;
      };
    };
    const draw = (data, width, height) => {
      const canvas = $("#live-canvas");
      if (!canvas) { socket?.close(); return; }
      const cell = Math.max(2, Math.min(16, Math.floor((canvas.parentElement.clientWidth - 2) / Math.max(1, width))));
      if (canvas.width !== width * cell || canvas.height !== height * cell) {
        canvas.width = width * cell;
        canvas.height = height * cell;
      }
      const ctx = canvas.getContext("2d");
      ctx.fillStyle = "#000";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      for (let y = 0, i = 4; y < height; y++) {
        for (let x = 0; x < width; x++, i += 3) {
          if (data[i] | data[i + 1] | data[i + 2]) {
            ctx.fillStyle = `rgb(${data[i]},${data[i + 1]},${data[i + 2]})`;
            ctx.fillRect(x * cell + 1, y * cell + 1, cell - 1, cell - 1);
          }
        }
      }
      $("#live-state").textContent = `${width} x ${height}, as drawn (before the brightness)`;
    };
    open();
    const watchdog = setInterval(() => {
      if (Date.now() - seen > 3000 || !$("#live-canvas")) {
        clearInterval(watchdog);
        socket?.close();
      }
    }, 1000);
    return { alive: () => { seen = Date.now(); if (!socket && $("#live-canvas")) open(); } };
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
