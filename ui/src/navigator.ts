// Part navigator: the feature tree, with status, rollback and a context menu.

import { ICONS } from "./dialogs";
import { clear, h } from "./dom";
import type { State } from "./types";

export interface NavigatorHost {
  edit(id: string): void;
  select(id: string | null): void;
  menu(id: string, x: number, y: number): void;
  rollTo(id: string | null): void;
}

export class Navigator {
  selected: string | null = null;

  constructor(private root: HTMLElement, private host: NavigatorHost) {}

  render(state: State): void {
    clear(this.root);
    const statuses = new Map(state.features.map((f) => [f.id, f]));
    const sketchStatus = new Map(state.sketches.map((s) => [s.sketch.id, s]));
    let afterRollback = false;
    const doc = state.document;

    this.root.append(h("li", { class: "nav-part", title: "The part" }, `▣ ${state.name}`));
    if (doc.parameters.length) {
      this.root.append(h("li", { class: "nav-params", title: "Tools ▸ Parameters" }, `ƒ Parameters (${doc.parameters.length})`));
    }
    if (!doc.features.length) {
      this.root.append(h("li", { class: "nav-empty" }, "Start with a sketch (S) or a primitive."));
    }
    for (const f of doc.features) {
      const st = statuses.get(f.id);
      let detail = "";
      let cls: string = st?.state ?? "ok";
      if (f.type === "sketch") {
        const s = sketchStatus.get(f.id);
        if (s && s.state === "solved") detail = s.dof === 0 ? "" : ` · ${s.dof} DOF`;
        if (s && s.state !== "solved") cls = "failed";
      }
      if (afterRollback) cls = "rolled";
      const mark = f.suppressed ? "⊘" : st?.state === "failed" ? "✗" : "";
      const li = h("li", {
        class: `nav-item ${cls}${this.selected === f.id ? " selected" : ""}`,
        title: st?.message || `${f.type} ${f.id}`,
        draggable: false,
        onclick: () => { this.selected = this.selected === f.id ? null : f.id; this.host.select(this.selected); this.render(state); },
        ondblclick: () => this.host.edit(f.id),
        oncontextmenu: (e: MouseEvent) => { e.preventDefault(); this.selected = f.id; this.host.select(f.id); this.render(state); this.host.menu(f.id, e.clientX, e.clientY); },
      },
        h("span", { class: "nav-icon" }, ICONS[f.type] ?? "•"),
        h("span", { class: "nav-name" }, f.name ? `${f.name}` : f.id),
        f.name ? h("span", { class: "nav-sub" }, f.id) : null,
        detail ? h("span", { class: "nav-sub" }, detail) : null,
        mark ? h("span", { class: "nav-mark" }, mark) : null);
      this.root.append(li);
      if (state.rollback === f.id) {
        afterRollback = true;
        this.root.append(h("li", { class: "nav-rollback", title: "Rolled back here. Click to roll forward to the end.", onclick: () => this.host.rollTo(null) },
          "⏸ rolled back — click to roll to end"));
      }
    }
  }
}
