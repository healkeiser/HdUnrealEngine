// Mermaid as hdUE draws it (overrides/main.html loads this after Mermaid, before the theme). The theme calls
// mermaid.initialize with its own settings and colours (the --md-mermaid-* variables, theme.css); this adds Inter,
// rounded nodes, smooth edges and Lucide's icons, fetched the first time a diagram names one.
(function () {
  if (!window.mermaid) return;
  // Lucide, under its own name for icon nodes (A@{ icon: "lucide:camera" }) and under "fal", one of the Font Awesome
  // prefixes Mermaid reads inside a label: "fal:fa-camera Camera" draws Lucide's camera before the text.
  const lucide = fetch("https://cdn.jsdelivr.net/npm/@iconify-json/lucide@1.2.145/icons.json").then((r) => r.json());
  mermaid.registerIconPacks([
    { name: "lucide", loader: () => lucide },
    { name: "fal", loader: () => lucide },
  ]);
  const CSS = `
    .node rect, .node polygon, .node circle, .node path.basic { stroke-width: 1px; }
    .node rect { rx: 8px; ry: 8px; }
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
    .node :is(rect, path, polygon) { fill: var(--md-mermaid-node-bg-color) !important; }
    .node.host :is(rect, path, polygon) { stroke: var(--hd-dg-host) !important; }
    .node.hdue :is(rect, path, polygon) { stroke: var(--hd-dg-delegate) !important; }
    .node.unreal :is(rect, path, polygon) { stroke: var(--hd-dg-unreal) !important; }
    .node.output :is(rect, path, polygon) { fill: var(--hd-dg-gpu-bg) !important; stroke: var(--hd-dg-gpu) !important; }
    .label-icon { margin-right: 0.15em; vertical-align: -0.15em; color: var(--hd-text-muted); }
    .host .label-icon { color: var(--hd-dg-host); }
    .hdue .label-icon { color: var(--hd-dg-delegate); }
    .unreal .label-icon { color: var(--hd-dg-unreal); }
    .output .label-icon { color: var(--hd-dg-gpu); }
    .cluster-label, .cluster .nodeLabel { color: var(--md-mermaid-label-fg-color); font-weight: 600; }
  `;
  const initialize = mermaid.initialize.bind(mermaid);
  mermaid.initialize = (config) =>
    initialize({
      ...config,
      fontFamily: "Inter, ui-sans-serif, system-ui, sans-serif",
      themeCSS: `${config.themeCSS || ""}${CSS}`,
      flowchart: { ...(config.flowchart || {}), curve: "basis", padding: 14, nodeSpacing: 36, rankSpacing: 44 },
    });
})();
