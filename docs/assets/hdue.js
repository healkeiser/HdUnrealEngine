// Setting comparisons (scripts/render_docs_images.py): <div class="hdue-figure" data-figure="<name>"></div> becomes a
// before/after slider for two images and, for more, a picture with a strip of thumbnails. Images and captions come from
// images/settings/, found next to this script so the page's depth does not matter.
(function () {
  const base = new URL("../images/settings/", document.currentScript.src);
  let figures = null;
  // Lucide's chevrons-left-right, the slider's grip.
  const CHEVRONS =
    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" ' +
    'stroke-linejoin="round" aria-hidden="true"><path d="m9 7-5 5 5 5"/><path d="m15 7 5 5-5 5"/></svg>';

  function slider(root, name, captions) {
    root.classList.add("hdue-compare");
    // The frame takes the image's own size; the figure around it is the band that spans the text column.
    root.innerHTML =
      `<div class="hdue-frame">` +
      `<img src="${new URL(`${name}/0.jpg`, base)}" alt="${captions[0]}">` +
      `<div class="hdue-after"><img src="${new URL(`${name}/1.jpg`, base)}" alt="${captions[1]}"></div>` +
      `<div class="hdue-handle"><span class="hdue-knob">${CHEVRONS}</span></div>` +
      `<span class="hdue-label hdue-left">${captions[0]}</span><span class="hdue-label hdue-right">${captions[1]}</span>` +
      `<input type="range" min="0" max="100" value="50" aria-label="${captions[0]} / ${captions[1]}">` +
      `</div>`;
    const frame = root.querySelector(".hdue-frame");
    const input = root.querySelector("input");
    const set = () => frame.style.setProperty("--split", `${input.value}%`);
    input.addEventListener("input", set);
    set();
  }

  // More than two values: the images stacked, the selected one shown with its caption, and a strip of thumbnails to
  // pick a value; swipes and the arrow keys step through them.
  function stepper(root, name, captions) {
    root.classList.add("hdue-steps");
    const src = (i) => new URL(`${name}/${i}.jpg`, base);
    root.innerHTML =
      `<div class="hdue-stage" tabindex="0" aria-label="${captions.length} values, arrow keys to change">` +
      captions.map((c, i) => `<img src="${src(i)}" alt="${c}"${i ? ' loading="lazy"' : ""}>`).join("") +
      `<span class="hdue-label hdue-left" aria-live="polite"></span></div>` +
      `<div class="hdue-thumbs">` +
      captions
        .map(
          (c, i) =>
            `<button type="button" aria-label="${c}"><img src="${src(i)}" alt="" loading="lazy"><span>${c}</span></button>`,
        )
        .join("") +
      `</div>`;
    const stage = root.querySelector(".hdue-stage");
    const images = stage.querySelectorAll("img");
    const strip = root.querySelector(".hdue-thumbs");
    const buttons = strip.querySelectorAll("button");
    let current = 0;
    const show = (i) => {
      current = Math.max(0, Math.min(captions.length - 1, i));
      images.forEach((img, j) => img.classList.toggle("hdue-current", j === current));
      buttons.forEach((b, j) => b.setAttribute("aria-pressed", String(j === current)));
      // Centre the selected thumbnail in the strip, without scrolling the page (scrollIntoView would).
      const b = buttons[current];
      strip.scrollTo({ left: b.offsetLeft - strip.offsetLeft - (strip.clientWidth - b.clientWidth) / 2, behavior: "smooth" });
      stage.querySelector(".hdue-label").textContent = captions[current];
    };
    buttons.forEach((b, i) => b.addEventListener("click", () => show(i)));
    stage.addEventListener("keydown", (e) => {
      if (e.key === "ArrowRight" || e.key === "ArrowLeft") {
        e.preventDefault();
        show(current + (e.key === "ArrowRight" ? 1 : -1));
      }
    });
    // A horizontal swipe of 30 px or more steps one value; vertical moves scroll the page (touch-action: pan-y).
    let startX = null;
    stage.addEventListener("pointerdown", (e) => (startX = e.clientX));
    stage.addEventListener("pointerup", (e) => {
      if (startX !== null && Math.abs(e.clientX - startX) >= 30) show(current + (e.clientX < startX ? 1 : -1));
      startX = null;
    });
    stage.addEventListener("pointercancel", () => (startX = null));
    show(0);
  }

  async function build() {
    const roots = document.querySelectorAll(".hdue-figure:not([data-built])");
    if (!roots.length) return;
    figures = figures || (await (await fetch(new URL("figures.json", base))).json());
    roots.forEach((root) => {
      const name = root.dataset.figure;
      const captions = figures[name];
      if (!captions) return;
      root.dataset.built = "";
      (captions.length === 2 ? slider : stepper)(root, name, captions);
    });
  }

  // Fifteen tabs do not fit a narrow window: the bar scrolls sideways (the wheel scrolls it too), and a fade on either
  // edge shows there is more. Instant navigation may swap the bar: the flag keeps one set of listeners per bar.
  function tabs() {
    const bar = document.querySelector(".md-tabs");
    const list = bar && bar.querySelector(".md-tabs__list");
    if (!list || list.dataset.hdueTabs) return;
    list.dataset.hdueTabs = "";
    const update = () => {
      const end = list.scrollWidth - list.clientWidth;
      bar.classList.toggle("hdue-more-left", list.scrollLeft > 1);
      bar.classList.toggle("hdue-more-right", list.scrollLeft < end - 1);
    };
    list.addEventListener("scroll", update, { passive: true });
    list.addEventListener(
      "wheel",
      (e) => {
        if (list.scrollWidth <= list.clientWidth || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
        e.preventDefault();
        list.scrollLeft += e.deltaY;
      },
      { passive: false },
    );
    new ResizeObserver(update).observe(list);
    // The open tab in view, as the theme scrolls the menu to the open page.
    const active = list.querySelector(".md-tabs__item--active");
    if (active) list.scrollLeft = active.offsetLeft - (list.clientWidth - active.clientWidth) / 2;
    update();
  }

  // The open tab's pill (theme.css .hdue-tab-pill) slides from the tab left to the one opened. Instant navigation
  // redraws the tab bar, so the pill starts where the last bar had it.
  let last = null;
  function pill() {
    const list = document.querySelector(".md-tabs__list");
    const active = list && list.querySelector(".md-tabs__item--active");
    if (!active) return;
    list.classList.add("hdue-glide");
    const box = list.querySelector(".hdue-tab-pill") || list.appendChild(document.createElement("span"));
    box.className = "hdue-tab-pill";
    box.setAttribute("aria-hidden", "true");
    const place = (at) => {
      box.style.width = `${at.width}px`;
      box.style.height = `${at.height}px`;
      box.style.translate = `${at.left}px ${at.top}px`;
    };
    const now = { left: active.offsetLeft, top: active.offsetTop, width: active.offsetWidth, height: active.offsetHeight };
    if (last) {
      box.style.transition = "none";
      place(last);
      box.getBoundingClientRect();
      box.style.transition = "";
    }
    place(now);
    last = now;
    // The web font lands after the first measure and widens every tab: follow the open tab's size.
    if (!list.dataset.hduePill) {
      list.dataset.hduePill = "";
      new ResizeObserver(() => {
        const at = list.querySelector(".md-tabs__item--active");
        if (!at) return;
        last = { left: at.offsetLeft, top: at.offsetTop, width: at.offsetWidth, height: at.offsetHeight };
        place(last);
      }).observe(list);
    }
  }

  // The gallery's charts (scripts/render_gallery.py writes images/gallery/gallery.json): render time per frame on a log
  // scale, and brightness against Karma CPU. Chart.js is fetched only by a page with a chart; the charts take the
  // page's colours and are drawn again when the reader switches between light and dark.
  const gallery = new URL("../images/gallery/", document.currentScript.src);
  const CHART_JS = "https://cdn.jsdelivr.net/npm/chart.js@4.4.6/dist/chart.umd.min.js";
  // The README charts' series (scripts/gallery_charts.py): hdUE blue, Karma orange, a mark per renderer.
  const SERIES = {
    lumen: { color: "#3987e5", point: "circle" },
    pathtraced: { color: "#1f5fae", point: "rectRot" },
    xpu: { color: "#f08a4b", point: "triangle" },
    karma: { color: "#d95926", point: "rect" },
  };
  let chartLib = null;
  let chartData = null;
  const drawn = [];

  const seconds = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s.toFixed(s < 10 ? 2 : 1)} s`);
  const tick = (s) => (s < 1 ? `${Math.round(s * 1000)} ms` : `${s} s`);

  function chartColors() {
    const style = getComputedStyle(document.body);
    const read = (name) => style.getPropertyValue(name).trim();
    return {
      text: read("--md-default-fg-color--light"),
      strong: read("--md-default-fg-color"),
      grid: read("--md-default-fg-color--lightest"),
      font: read("--md-text-font-family") || "Inter, sans-serif",
    };
  }

  function chartConfig(kind, data, c) {
    const scenes = data.sets.flatMap((set, i) =>
      set.scenes.map((scene) => ({ ...scene, label: i ? `${set.title.split(":")[0]}: ${scene.title}` : scene.title })),
    );
    const renderers = data.renderers.filter((r) => kind === "times" || r.key !== "karma");
    const value = (scene, key) => {
      const render = scene.renders[key];
      if (!render) return null;
      if (kind === "times") return render.seconds > 0 ? render.seconds : null;
      const reference = scene.renders.karma;
      return scene.compared && reference ? render.luminance / reference.luminance : null;
    };
    const datasets = renderers.map((r) => ({
      label: r.label,
      data: scenes.map((scene) => value(scene, r.key)),
      backgroundColor: SERIES[r.key].color,
      borderColor: SERIES[r.key].color,
      pointStyle: SERIES[r.key].point,
      pointRadius: 5,
      pointHoverRadius: 7,
      showLine: false,
      borderRadius: 2,
      barPercentage: 0.9,
      categoryPercentage: 0.8,
    }));
    const axis = { color: c.text, font: { family: c.font, size: 11 } };
    // Karma CPU is the reference: a dashed line at 1 in the brightness chart.
    const reference = {
      id: "hdueReference",
      beforeDatasetsDraw(chart) {
        if (kind !== "accuracy") return;
        const { ctx, chartArea, scales } = chart;
        const x = scales.x.getPixelForValue(1);
        ctx.save();
        ctx.strokeStyle = c.text;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        ctx.moveTo(x, chartArea.top);
        ctx.lineTo(x, chartArea.bottom);
        ctx.stroke();
        ctx.restore();
      },
    };
    return {
      type: kind === "times" ? "bar" : "line",
      data: { labels: scenes.map((s) => s.label), datasets },
      plugins: [reference],
      options: {
        indexAxis: "y",
        maintainAspectRatio: false,
        animation: { duration: 400 },
        interaction: { mode: "nearest", axis: "y", intersect: false },
        scales: {
          x:
            kind === "times"
              ? {
                  type: "logarithmic",
                  title: { display: true, text: "Time per frame (log scale)", ...axis },
                  ticks: { ...axis, maxRotation: 0, callback: (v) => ([0.01, 0.1, 1, 10, 100].includes(v) ? tick(v) : "") },
                  grid: { color: c.grid },
                }
              : {
                  title: { display: true, text: "Mean brightness relative to Karma CPU", ...axis },
                  ticks: { ...axis, callback: (v) => `${Number(v).toFixed(2)}×` },
                  grid: { color: c.grid },
                },
          y: { ticks: { ...axis, color: c.strong }, grid: { display: false } },
        },
        plugins: {
          legend: {
            position: "top",
            align: "start",
            labels: {
              color: c.strong,
              font: { family: c.font, size: 12 },
              usePointStyle: true,
              boxWidth: 7,
              boxHeight: 7,
              padding: 16,
            },
          },
          title: {
            display: true,
            align: "start",
            text: kind === "times" ? "Render time per frame" : "Brightness against Karma CPU (the reference, 1.00×)",
            color: c.strong,
            font: { family: c.font, size: 14, weight: "600" },
            padding: { bottom: 4 },
          },
          tooltip: {
            callbacks: {
              label: (item) =>
                `${item.dataset.label}: ${kind === "times" ? seconds(item.raw) : `${item.raw.toFixed(2)}× Karma CPU`}`,
            },
          },
        },
      },
      count: scenes.length,
    };
  }

  function drawCharts() {
    drawn.splice(0).forEach((chart) => chart.destroy());
    const colors = chartColors();
    document.querySelectorAll(".hdue-chart").forEach((root) => {
      const { count, ...config } = chartConfig(root.dataset.chart, chartData, colors);
      const bars = root.dataset.chart === "times" ? 4 : 1;
      root.style.height = `${count * (bars === 4 ? 2.1 : 1.3) + 5}rem`;
      root.replaceChildren(document.createElement("canvas"));
      drawn.push(new window.Chart(root.firstChild, config));
    });
  }

  async function charts() {
    if (!document.querySelector(".hdue-chart")) return;
    chartLib =
      chartLib ||
      new Promise((resolve, reject) => {
        const script = document.createElement("script");
        script.src = CHART_JS;
        script.integrity = "sha384-Sse/HDqcypGpyTDpvZOJNnG0TT3feGQUkF9H+mnRvic+LjR+K1NhTt8f51KIQ3v3";
        script.crossOrigin = "anonymous";
        script.onload = resolve;
        script.onerror = reject;
        document.head.append(script);
      });
    const json = chartData ? null : fetch(new URL("gallery.json", gallery)).then((r) => r.json());
    await chartLib;
    chartData = chartData || (await json);
    drawCharts();
  }
  new MutationObserver(() => drawn.length && drawCharts()).observe(document.body, {
    attributes: true,
    attributeFilter: ["data-md-color-scheme"],
  });

  // The light and dark switch leaves the header for the page's corner (theme.css .hdue-palette). The body outlives
  // instant navigation and the theme bound its inputs at load, so moving the form keeps it working.
  const palette = document.querySelector('.md-header [data-md-component="palette"]');
  if (palette) {
    palette.classList.add("hdue-palette");
    document.body.append(palette);
  }

  // A click on a diagram shows it full screen, scaled to fit (the theme draws it in a closed shadow root, so `zoom`
  // on its host scales it); Esc or a second click closes it. One listener on the document outlives navigation.
  document.addEventListener("click", (event) => {
    const diagram = event.target.closest(".md-typeset .mermaid");
    if (!diagram || !diagram.requestFullscreen) return;
    if (document.fullscreenElement) return void document.exitFullscreen();
    const box = diagram.getBoundingClientRect();
    diagram.requestFullscreen().then(() => {
      diagram.style.zoom = String(Math.max(1, 0.9 * Math.min(innerWidth / box.width, innerHeight / box.height)));
    });
  });
  document.addEventListener("fullscreenchange", () => {
    if (!document.fullscreenElement) document.querySelectorAll(".mermaid").forEach((d) => (d.style.zoom = ""));
  });

  // Formulas (pymdownx.arithmatex, generic): MathJax, as Zensical's math guide sets it up, fetched by the first page
  // that has one and run again on each page instant navigation shows.
  let mathjax = null;
  function math() {
    if (!document.querySelector(".arithmatex")) return;
    if (!mathjax) {
      window.MathJax = {
        tex: {
          inlineMath: [["\\(", "\\)"]],
          displayMath: [["\\[", "\\]"]],
          processEscapes: true,
          processEnvironments: true,
        },
        options: { ignoreHtmlClass: ".*|", processHtmlClass: "arithmatex" },
        startup: { typeset: false },
      };
      mathjax = new Promise((resolve, reject) => {
        const script = document.createElement("script");
        script.src = "https://cdn.jsdelivr.net/npm/mathjax@3.2.2/es5/tex-mml-chtml.js";
        script.integrity = "sha384-Wuix6BuhrWbjDBs24bXrjf4ZQ5aFeFWBuKkFekO2t8xFU0iNaLQfp2K6/1Nxveei";
        script.crossOrigin = "anonymous";
        script.onload = () => window.MathJax.startup.promise.then(resolve);
        script.onerror = reject;
        document.head.append(script);
      });
    }
    mathjax.then(() => {
      MathJax.startup.output.clearCache();
      MathJax.typesetClear();
      MathJax.texReset();
      MathJax.typesetPromise();
    });
  }

  // The theme fills in the repository's stars and forks only in the widgets present at load; the contents column's
  // copy (overrides/partials/toc.html) is redrawn by each navigation, so it takes them from the header's, which stays.
  function facts() {
    const source = document.querySelector(".md-header__source .md-source__facts");
    const target = document.querySelector(".hdue-toc-meta__source .md-source__repository");
    if (!source || !target || target.querySelector(".md-source__facts")) return;
    target.append(source.cloneNode(true));
  }
  // The header's facts may arrive after the first page is shown (the theme fetches them).
  const repository = document.querySelector(".md-header__source .md-source__repository");
  if (repository) new MutationObserver(facts).observe(repository, { childList: true, subtree: true });

  // The pinned panel's frame (theme.css) starts under the header, whose height follows the tab bar and the font.
  const header = document.querySelector(".md-header");
  if (header) {
    const height = () => document.documentElement.style.setProperty("--hd-header", `${header.offsetHeight}px`);
    new ResizeObserver(height).observe(header);
    height();
  }

  // Instant navigation swaps pages without reloading scripts: rebuild on each page it shows, and let the new text rise
  // in (not on the first load).
  let loaded = false;
  const page = () => {
    tabs();
    pill();
    build();
    charts();
    facts();
    math();
    if (loaded) document.querySelector(".md-content__inner")?.classList.add("hdue-page-in");
    loaded = true;
  };
  if (window.document$) window.document$.subscribe(page);
  else document.addEventListener("DOMContentLoaded", page);
})();
