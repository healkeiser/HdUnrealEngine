// Setting comparisons (scripts/render_docs_images.py): <div class="hdue-figure" data-figure="<name>"></div> becomes a
// before/after slider for two images and a value scrubber for more. Images and captions come from
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

  function scrubber(root, name, captions) {
    root.classList.add("hdue-scrub");
    root.innerHTML =
      captions.map((c, i) => `<img src="${new URL(`${name}/${i}.jpg`, base)}" alt="${c}" loading="lazy">`).join("") +
      `<div class="hdue-controls"><input type="range" min="0" max="${captions.length - 1}" step="1" value="0" ` +
      `aria-label="Setting value"><output></output></div>` +
      `<div class="hdue-ticks">${captions.map((c) => `<span>${c}</span>`).join("")}</div>`;
    const input = root.querySelector("input");
    const images = root.querySelectorAll("img");
    const show = () => {
      images.forEach((img, i) => img.classList.toggle("hdue-current", i === Number(input.value)));
      root.querySelector("output").textContent = captions[input.value];
    };
    input.addEventListener("input", show);
    show();
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
      (captions.length === 2 ? slider : scrubber)(root, name, captions);
    });
  }

  // Instant navigation swaps pages without reloading scripts: rebuild on each page it shows.
  if (window.document$) window.document$.subscribe(build);
  else document.addEventListener("DOMContentLoaded", build);
})();
