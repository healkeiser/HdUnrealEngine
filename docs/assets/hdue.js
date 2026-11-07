// Setting comparisons (scripts/render_docs_images.py): <div class="hdue-figure" data-figure="<name>"></div> becomes a
// before/after slider for two images and, for more, a picture with a strip of thumbnails. Images and captions come from
// images/settings/, found next to this script so the page's depth does not matter.
(function () {
  const base = new URL("../images/settings/", document.currentScript.src);
  let figures = null;

  function slider(root, name, captions) {
    root.classList.add("hdue-compare");
    root.innerHTML =
      `<img src="${new URL(`${name}/0.jpg`, base)}" alt="${captions[0]}">` +
      `<div class="hdue-after"><img src="${new URL(`${name}/1.jpg`, base)}" alt="${captions[1]}"></div>` +
      `<div class="hdue-handle"></div>` +
      `<span class="hdue-label hdue-left">${captions[0]}</span><span class="hdue-label hdue-right">${captions[1]}</span>` +
      `<input type="range" min="0" max="100" value="50" aria-label="${captions[0]} / ${captions[1]}">`;
    const input = root.querySelector("input");
    const set = () => root.style.setProperty("--split", `${input.value}%`);
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

  // Instant navigation swaps pages without reloading scripts: rebuild on each page it shows.
  if (window.document$) window.document$.subscribe(build);
  else document.addEventListener("DOMContentLoaded", build);
})();
