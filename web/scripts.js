// The Scripts page of the web UI (docs/ui/web/web-app.md section 4, docs/glow.md): animations in
// Glow, written here and compiled on the device. Loaded by app.js with the page (only in a firmware
// with CONFIG_PX_SCRIPTS); uses app.js' helpers (api, render, card, esc, busy, toast) and leds.js'
// preview. The page parses nothing: the device compiles and says where a script is wrong.
"use strict";

window.pxScripts = (() => {
  // the list of the animations comes with GET /api/leds only: the answers and pushes keep it
  const keep = (old, fresh) => (fresh.effects ? fresh : { ...fresh, effects: old.effects });
  // the examples of web/glow
  const examples = [
    ["plasma-waves", "Plasma waves"], ["fire-grid", "Fire grid"], ["bouncing-balls", "Bouncing balls"],
    ["beat-rings", "Beat rings"], ["lava-lamp", "Lava lamp"], ["fire-2012-glow", "Fire 2012 (Glow)"],
    ["octopus-glow", "Octopus (Glow)"], ["julia-glow", "Julia (Glow)"], ["fireworks-1d-glow", "Fireworks 1D (Glow)"],
    ["dna-spiral-glow", "DNA spiral (Glow)"], ["sun-radiation-glow", "Sun radiation (Glow)"],
    ["drift-rose-glow", "Drift rose (Glow)"],
  ];
  const empty = 'anim "My animation"\n\npixel {\n  hsv(u + t / 10, 1, 1)\n}\n';

  // a POST with the source as the body (text, not a form); JSON answers, a 401 leads to the login
  async function postText(path, text) {
    const response = await fetch(path, { method: "POST", body: text, cache: "no-store",
      headers: { "Content-Type": "text/plain; charset=utf-8" } });
    const body = await response.json().catch(() => ({}));
    if (response.status === 401) {
      go("login");
      throw new HttpError(401, "login required");
    }
    if (!response.ok) throw new HttpError(response.status, body.error || response.statusText);
    return body;
  }

  const reference = `
    <dl class="facts small mb-0">
      <dt>Blocks</dt><dd><code>init { }</code> once, <code>frame { }</code> every frame, <code>pixel { }</code> every pixel</dd>
      <dt>Declared</dt><dd><code>anim "Name"</code>, <code>param size = 0.5 [0 .. 1] "Size"</code> (a slider),
        <code>var a, p[16]</code>, <code>grid g</code>, <code>fn f(a, b) { return a * b }</code>, <code>sound required</code></dd>
      <dt>Values</dt><dd><code>t dt</code> seconds at the speed, <code>W H N</code>, <code>hue sat</code> of the color,
        <code>level beat bass mid high band(k)</code> the sound; per pixel <code>x y</code> (pixels), <code>u v</code> (0..1), <code>i</code></dd>
      <dt>Math</dt><dd><code>+ - * / %</code>, <code>&lt; &lt;= &gt; &gt;= == !=</code>, <code>and or not</code>, <code>c ? a : b</code>,
        <code>abs floor ceil frac round sqrt min max clamp mix step smooth hypot atan2</code></dd>
      <dt>Waves</dt><dd>a turn is 1: <code>sin cos</code> (-1..1), <code>wave tri saw</code> (0..1), <code>square(x, duty)</code>,
        <code>time(p)</code>, <code>noise(x, y, z) rand() rand(n) hash(a, b)</code></dd>
      <dt>Color</dt><dd><code>hsv(h, s, v) rgb(r, g, b) pal(p, v) col(v) heat(v)</code>: the pixel's; in init and frame the pen's</dd>
      <dt>Drawing</dt><dd>init and frame: <code>fade(a) blur(a) clear() dot(x, y) line(x1, y1, x2, y2) disc(x, y, r) blob(x, y, r)</code></dd>
      <dt>Control</dt><dd><code>if else</code>, <code>for k in 0 .. 10</code>, <code>repeat n</code>, <code>while</code> (not per pixel),
        <code>break continue</code>; numbers are 16.16, from -32768 to 32767</dd>
    </dl>`;

  // the editor's look: here, not in app.css, so a firmware without scripts has none of it
  const style = `
    .script-editor { display: flex; height: 22rem; overflow: hidden; resize: vertical; }
    .script-gutter { margin: 0; padding: .5rem .4rem; min-width: 2.8rem; text-align: right; overflow: hidden;
      user-select: none; color: var(--bs-secondary-color); background: var(--bs-tertiary-bg);
      font: .85rem/1.45 var(--bs-font-monospace); }
    .script-text { flex: 1; border: 0; padding: .5rem; resize: none; outline: none; white-space: pre; overflow: auto;
      tab-size: 2; color: inherit; background: transparent; font: .85rem/1.45 var(--bs-font-monospace); }`;

  async function page() {
    if (!$("#script-style")) document.head.insertAdjacentHTML("beforeend", `<style id="script-style">${style}</style>`);
    const view = render(`<h1>Scripts</h1>
      <div id="scripts-live"></div><div id="scripts-editor"></div><div id="scripts-sliders"></div>
      <div id="scripts-list">Loading ...</div><div id="scripts-help"></div>`);
    let list = await api("/api/scripts");
    let leds = await api("/api/leds");
    // what the editor holds: the slug it was loaded from or saved as ("" new) and its text then
    let open = { slug: "", text: empty };
    window.pxLeds.preview("#scripts-live");

    $("#scripts-editor").innerHTML = card("Editor", `
      <div class="row g-2 mb-2">
        <div class="col-sm-5"><label class="form-label" for="script-slug">File name</label>
          <input id="script-slug" class="form-control" placeholder="my-animation" pattern="[a-z0-9-]{1,23}"
            autocapitalize="off" spellcheck="false"></div>
        <div class="col-sm-7"><label class="form-label" for="script-example">Start from</label>
          <select id="script-example" class="form-select">
            <option value="">an empty script</option>
            ${examples.map(([name, title]) => `<option value="${name}">the example "${esc(title)}"</option>`).join("")}
          </select></div>
      </div>
      <div class="script-editor border rounded mb-2">
        <pre id="script-gutter" class="script-gutter" aria-hidden="true"></pre>
        <textarea id="script-text" class="script-text" wrap="off" spellcheck="false" autocapitalize="off"
          aria-label="the script"></textarea>
      </div>
      <div id="script-error" class="small text-danger mb-2" hidden></div>
      <div class="d-flex gap-2 flex-wrap justify-content-end">
        <button id="script-delete" class="btn btn-outline-danger me-auto" type="button">Delete</button>
        <button id="script-revert" class="btn btn-outline-secondary" type="button">Revert</button>
        <button id="script-run" class="btn btn-outline-primary" type="button" title="Ctrl+Enter">Run</button>
        <button id="script-save" class="btn btn-primary" type="button" title="Ctrl+S">Save</button>
      </div>
      <div class="form-text">Run shows the script on the LEDs without saving it; Save keeps it as an animation
        (in the list of animations, presets and Home Assistant). Tab indents, Ctrl+Enter runs.</div>`);
    $("#scripts-help").innerHTML = card("The language", `
      <details><summary class="mb-2">Short reference (the whole one: docs/glow.md)</summary>${reference}</details>`);

    const text = $("#script-text");
    const gutter = $("#script-gutter");
    const slugInput = $("#script-slug");
    // the line numbers beside the text, the line of an error marked
    let errorLine = 0;
    const drawGutter = () => {
      const lines = text.value.split("\n").length;
      gutter.innerHTML = Array.from({ length: lines }, (_, n) =>
        n + 1 === errorLine ? `<span class="text-danger fw-bold">${n + 1}</span>` : String(n + 1)).join("\n");
      gutter.scrollTop = text.scrollTop;
    };
    const setText = (value) => {
      text.value = value;
      text.scrollTop = 0;
      showError(null);
    };
    text.oninput = drawGutter;
    text.onscroll = () => (gutter.scrollTop = text.scrollTop);
    text.onkeydown = (e) => {
      if (e.key === "Tab" && !e.shiftKey) {
        e.preventDefault();
        text.setRangeText("  ", text.selectionStart, text.selectionEnd, "end");
        drawGutter();
      } else if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        $("#script-run").click();
      } else if (e.key === "s" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        $("#script-save").click();
      }
    };

    // an error of the device: "line 3, column 5: ...", that line selected
    const showError = (error) => {
      const box = $("#script-error");
      errorLine = error && error.line ? error.line : 0;
      box.hidden = !error;
      box.textContent = !error ? "" : error.line ? `line ${error.line}, column ${error.column}: ${error.text}` : error.text;
      drawGutter();
      if (errorLine) {
        const lines = text.value.split("\n");
        const start = lines.slice(0, errorLine - 1).reduce((n, l) => n + l.length + 1, 0);
        text.focus();
        text.setSelectionRange(start, start + (lines[errorLine - 1] || "").length);
        text.scrollTop = Math.max(0, (errorLine - 4) * parseFloat(getComputedStyle(text).lineHeight || "20"));
      }
    };

    const load = async (slug) => {
      const response = await fetch(`/api/scripts/source?slug=${encodeURIComponent(slug)}`, { cache: "no-store" });
      if (!response.ok) throw new HttpError(response.status, "the script could not be loaded");
      open = { slug, text: await response.text() };
      slugInput.value = slug;
      $("#script-example").value = "";
      setText(open.text);
      const script = list.scripts.find((x) => x.slug === slug);
      if (script && !script.ok) showError(script.error);
    };
    $("#script-example").onchange = async (e) => {
      const name = e.target.value;
      if (text.value !== open.text && !confirm("Replace the text in the editor?")) {
        e.target.value = "";
        return;
      }
      let value = empty;
      if (name) {
        const response = await fetch(`glow/${name}.glow`, { cache: "no-store" });
        value = response.ok ? await response.text() : empty;
      }
      open = { slug: "", text: value };
      if (!slugInput.value || list.scripts.some((x) => x.slug === slugInput.value)) slugInput.value = name;
      setText(value);
    };

    const showAnswer = (answer, done) => {
      showError(answer.error);
      if (answer.ok) toast(done, "success");
    };
    $("#script-run").onclick = (e) => busy(e.target, async () => {
      showAnswer(await postText("/api/scripts/run", text.value), "running (not saved)");
    });
    $("#script-save").onclick = (e) => busy(e.target, async () => {
      const slug = slugInput.value.trim();
      if (!/^[a-z0-9-]{1,23}$/.test(slug) || slug === "run") {
        slugInput.focus();
        throw new Error("a file name: 1 to 23 of a-z, 0-9 and -");
      }
      const answer = await postText(`/api/scripts/save?slug=${encodeURIComponent(slug)}`, text.value);
      open = { slug, text: text.value };
      showAnswer(answer, "saved");
      if (!answer.ok) toast("saved, but it does not compile: the version before runs on", "warning");
      // the saved one on the LEDs (also after a run of it)
      if (answer.ok) leds = keep(leds, await api("/api/leds", { effect: slug }));
      await refresh();
    });
    $("#script-revert").onclick = (e) => busy(e.target, async () => {
      if (open.slug) await load(open.slug);
      else setText(open.text);
    });
    $("#script-delete").onclick = (e) => busy(e.target, async () => {
      const slug = open.slug || slugInput.value.trim();
      if (!list.scripts.some((x) => x.slug === slug)) throw new Error("not saved: nothing to delete");
      if (!confirm(`Delete the script "${slug}"?`)) return;
      await api(`/api/scripts/delete?slug=${encodeURIComponent(slug)}`, {});
      open = { slug: "", text: empty };
      slugInput.value = "";
      setText(empty);
      toast("deleted", "success");
      await refresh();
    });

    // the scripts: each one's state and size; a click opens it
    const drawList = () => {
      const rows = list.scripts.map((x) => `
        <button type="button" class="list-group-item list-group-item-action d-flex gap-2 align-items-center" data-slug="${esc(x.slug)}">
          <span class="flex-fill"><span class="fw-semibold">${esc(x.name)}</span>
            <span class="small text-body-secondary">${esc(x.slug)}.glow, ${x.size} bytes${x.program ? `, program ${x.program} bytes` : ""}</span></span>
          ${x.ok ? "" : `<span class="badge text-bg-danger" title="${esc(x.error?.text || "")}">error</span>`}
          ${leds.effect === x.slug ? '<span class="badge text-bg-primary">shown</span>' : ""}
        </button>`).join("");
      $("#scripts-list").innerHTML = card(`Saved (${list.scripts.length} of ${list.slots})`, `
        ${rows ? `<div class="list-group mb-2">${rows}</div>` : '<p class="text-body-secondary">None yet: write one above, or start from an example.</p>'}
        <div class="form-text mb-0">The compiler took ${list.stack} bytes of stack at most.</div>`);
      document.querySelectorAll("#scripts-list [data-slug]").forEach((row) => {
        row.onclick = () => {
          if (text.value !== open.text && !confirm("Leave the text in the editor?")) return;
          load(row.dataset.slug).catch((e) => toast(e.message, "danger"));
        };
      });
    };

    // the sliders of the script the light shows (its params), and why it draws nothing
    const drawSliders = () => {
      const box = $("#scripts-sliders");
      if (!box) return;
      // the shown one, with its sliders
      const effect = leds.shown && leds.shown.script && leds.shown.slug === leds.effect ? leds.shown : null;
      const params = effect ? effect.params : [];
      const problem = leds.script_error ? `<p class="text-danger small">${esc(leds.script_error)}</p>` : "";
      if (!effect) {
        box.innerHTML = problem ? card("Shown", problem) : "";
        return;
      }
      if (box.dataset.effect === leds.effect && box.dataset.count === String(params.length) && !problem) {
        params.forEach((p, k) => {
          const input = $(`#script-p${k}`);
          if (input && document.activeElement !== input) input.value = String(p.slider);
          const value = $(`#script-p${k}-value`);
          if (value) value.textContent = +p.value.toFixed(3);
        });
        return;
      }
      box.dataset.effect = leds.effect;
      box.dataset.count = String(params.length);
      box.innerHTML = card(`Shown: ${effect.name}`, problem + (params.length ? params.map((p, k) => `
        <label class="form-label" for="script-p${k}">${esc(p.label)} <span id="script-p${k}-value" class="text-body-secondary">${+p.value.toFixed(3)}</span></label>
        <input id="script-p${k}" type="range" class="form-range mb-2" min="0" max="255" value="${p.slider}">`).join("")
        : '<p class="text-body-secondary small mb-0">No params: a line <code>param size = 0.5 [0 .. 1] "Size"</code> makes a slider.</p>'));
      params.forEach((p, k) => {
        const input = $(`#script-p${k}`);
        let timer = null;
        const send = () => api("/api/leds", { [`p${k}`]: input.value }).then((answer) => {
          leds = keep(leds, answer);
          drawSliders();
        }).catch((e) => toast(e.message, "danger"));
        input.oninput = () => {
          if (!timer) timer = setTimeout(() => { timer = null; send(); }, 150);
        };
        input.onchange = send;
      });
    };

    const refresh = async () => {
      list = await api("/api/scripts");
      drawList();
      drawSliders();
    };
    setText(empty);
    drawList();
    drawSliders();
    // open the script the light shows, else start empty
    if (list.scripts.some((x) => x.slug === leds.effect)) await load(leds.effect).catch(() => {});
    view.live("/api/scripts", (state) => {
      list = state;
      drawList();
    });
    view.live("/api/leds", (state) => {
      leds = keep(leds, state);
      drawSliders();
    });
  }

  return { page };
})();
