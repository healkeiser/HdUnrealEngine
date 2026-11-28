// Mermaid as hdUE draws it. overrides/main.html loads this on every page, before the theme: `mermaid` is a stand-in
// the theme takes for Mermaid itself. Instant navigation does not run a new page's scripts, so the first diagram the
// theme draws, on whichever page, loads the pinned Mermaid here, with Lucide's icons and hdUE's look (Inter, rounded
// nodes, smooth edges, the four classes' colours from theme.css) added to the theme's own settings.
(function () {
  const MERMAID = "https://cdn.jsdelivr.net/npm/mermaid@12.1.0/dist/mermaid.min.js";
  const INTEGRITY = "sha384-EbBpjO7rlR6eqZEcG7GaPpyk9H9WrMyPWX4d3KvPYltgt8Z8l0z6R56B1qP40pR4";
  const LUCIDE = "https://cdn.jsdelivr.net/npm/@iconify-json/lucide@1.2.145/icons.json";
  const CSS = `
    /* A node's own shape is its direct child: rules on deeper rects would reach the icons in its label. */
    .node > :is(rect, polygon, circle, path) { stroke-width: 1px; }
    .node > rect { rx: 8px; ry: 8px; }
    .cluster rect { rx: 10px; ry: 10px; stroke-dasharray: 4 3; }
    .edgePath .path, .flowchart-link { stroke-width: 1.25px; }
    /* Edge labels: small text on the frame's grey (Mermaid's default is a light grey block in both schemes). */
    .edgeLabel, .edgeLabel p, .edgeLabel span, .labelBkg {
      background-color: var(--md-mermaid-label-bg-color) !important;
      color: var(--md-mermaid-edge-color) !important;
      border-radius: 4px;
      font-size: 12px;
    }
    .edgeLabel rect { fill: var(--md-mermaid-label-bg-color) !important; opacity: 1 !important; }
    .edgeLabel p { padding: 1px 4px; }
    /* Icon nodes: a tile like the other nodes (.icon-shape2: its fill, then its outline), the Lucide icon in the accent
       (it draws in currentColor), the label plain text under it. */
    .icon-shape2 path:first-child { fill: var(--md-mermaid-node-bg-color); }
    .icon-shape2 path:last-child { stroke: var(--md-mermaid-node-fg-color); stroke-width: 1px; }
    .icon-shape svg { color: var(--hd-accent); }
    .icon-shape .label rect { fill: none !important; }
    .icon-shape .label, .icon-shape .label * { background: transparent !important; }
    .icon-shape .label p, .icon-shape .label span { color: var(--md-mermaid-label-fg-color) !important; font-size: 13px; }
    /* One look for every diagram (docs/developer/writing-docs.md): four classes, coloured from theme.css's --hd-dg-*
       (a classDef cannot name a variable). host is Houdini and USD, hdue the delegate, unreal Unreal Engine, output the
       pixels and files; a group is a tint with a line of its colour, a node the panel with a line of it, and the
       label's icon takes the colour too. */
    .cluster.host rect { fill: var(--hd-dg-host-bg) !important; stroke: var(--hd-dg-host) !important; }
    .cluster.hdue rect { fill: var(--hd-dg-delegate-bg) !important; stroke: var(--hd-dg-delegate) !important; }
    .cluster.unreal rect { fill: var(--hd-dg-unreal-bg) !important; stroke: var(--hd-dg-unreal) !important; }
    .cluster.output rect { fill: var(--hd-dg-gpu-bg) !important; stroke: var(--hd-dg-gpu) !important; }
    .node > :is(rect, path, polygon) { fill: var(--md-mermaid-node-bg-color) !important; }
    .node.host > :is(rect, path, polygon) { stroke: var(--hd-dg-host) !important; }
    .node.hdue > :is(rect, path, polygon) { stroke: var(--hd-dg-delegate) !important; }
    .node.unreal > :is(rect, path, polygon) { stroke: var(--hd-dg-unreal) !important; }
    .node.output > :is(rect, path, polygon) { fill: var(--hd-dg-gpu-bg) !important; stroke: var(--hd-dg-gpu) !important; }
    .label-icon { margin-right: 0.15em; vertical-align: -0.15em; color: var(--hd-text-muted); }
    /* Lucide draws in strokes of currentColor: the theme's node fill and line must not reach its parts. */
    .label-icon :is(path, rect, circle, line, polyline, polygon, ellipse) {
      fill: none !important;
      stroke: currentColor !important;
    }
    .host .label-icon { color: var(--hd-dg-host); }
    .hdue .label-icon { color: var(--hd-dg-delegate); }
    .unreal .label-icon { color: var(--hd-dg-unreal); }
    .output .label-icon { color: var(--hd-dg-gpu); }
    .cluster-label, .cluster .nodeLabel { color: var(--md-mermaid-label-fg-color); font-weight: 600; }
  `;
  let settings = {};
  let real = null;
  function load() {
    real =
      real ||
      new Promise((resolve, reject) => {
        const script = document.createElement("script");
        script.src = MERMAID;
        script.integrity = INTEGRITY;
        script.crossOrigin = "anonymous";
        script.onerror = reject;
        script.onload = () => {
          const mermaid = window.mermaid;
          // Lucide, under its own name for icon nodes (A@{ icon: "lucide:camera" }) and under "fal", one of the Font
          // Awesome prefixes Mermaid reads inside a label: "fal:fa-camera Camera" draws Lucide's camera before the text.
          const lucide = fetch(LUCIDE).then((r) => r.json());
          mermaid.registerIconPacks([
            { name: "lucide", loader: () => lucide },
            { name: "fal", loader: () => lucide },
          ]);
          mermaid.initialize({
            ...settings,
            fontFamily: "Inter, ui-sans-serif, system-ui, sans-serif",
            themeCSS: `${settings.themeCSS || ""}${CSS}`,
            flowchart: { ...(settings.flowchart || {}), curve: "basis", padding: 14, nodeSpacing: 36, rankSpacing: 44 },
          });
          resolve(mermaid);
        };
        document.head.append(script);
      });
    return real;
  }
  // The theme calls initialize once, then render for each diagram (and awaits it).
  window.mermaid = {
    initialize: (config) => (settings = config),
    render: async (id, text) => (await load()).render(id, text),
  };
})();
