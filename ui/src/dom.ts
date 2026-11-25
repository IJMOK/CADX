// Tiny DOM helpers. CADX uses no UI framework: the viewport is three.js and the
// rest is plain forms and lists, which these helpers keep short.

type Child = Node | string | number | null | undefined | false;
type Attrs = Record<string, unknown>;

export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Attrs | null = null,
  ...children: (Child | Child[])[]
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k.startsWith("on") && typeof v === "function") {
        el.addEventListener(k.slice(2).toLowerCase(), v as EventListener);
      } else if (k === "class") {
        el.className = String(v);
      } else if (k === "style" && typeof v === "object") {
        Object.assign(el.style, v);
      } else if (k in el && typeof v !== "string") {
        (el as unknown as Record<string, unknown>)[k] = v;
      } else if (k === "value" || k === "checked") {
        (el as unknown as Record<string, unknown>)[k] = v;
      } else {
        el.setAttribute(k, v === true ? "" : String(v));
      }
    }
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : String(c));
  }
  return el;
}

export const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

export function clear(el: HTMLElement): void {
  while (el.firstChild) el.removeChild(el.firstChild);
}

export function debounce<A extends unknown[]>(fn: (...a: A) => void, ms: number): (...a: A) => void {
  let t: number | undefined;
  return (...a: A) => {
    window.clearTimeout(t);
    t = window.setTimeout(() => fn(...a), ms);
  };
}

export const fmt = (v: number, digits = 3): string => {
  if (!Number.isFinite(v)) return String(v);
  if (Math.abs(v) < 0.5 * 10 ** -digits) return "0"; // no "-0"
  const s = v.toFixed(digits);
  return s.includes(".") ? s.replace(/\.?0+$/, "") : s;
};

/** A message toast in the corner. */
export function toast(text: string, kind: "info" | "error" | "ok" = "info", ms = 3500): void {
  let box = document.getElementById("toasts");
  if (!box) {
    box = h("div", { id: "toasts" });
    document.body.append(box);
  }
  const t = h("div", { class: `toast ${kind}` }, text);
  box.append(t);
  window.setTimeout(() => t.remove(), ms);
}

/** Modal dialog. Resolves with the button value, or null if dismissed. */
export function modal(title: string, body: Node, buttons: { label: string; value: string; primary?: boolean }[]): Promise<string | null> {
  return new Promise((resolve) => {
    const close = (v: string | null) => {
      overlay.remove();
      document.removeEventListener("keydown", onKey, true);
      resolve(v);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") { e.stopPropagation(); close(null); }
      if (e.key === "Enter" && !(e.target instanceof HTMLTextAreaElement)) {
        const p = buttons.find((b) => b.primary);
        if (p) { e.stopPropagation(); e.preventDefault(); close(p.value); }
      }
    };
    const overlay = h("div", { class: "modal-overlay", onmousedown: (e: MouseEvent) => { if (e.target === overlay) close(null); } },
      h("div", { class: "modal" },
        h("h3", null, title),
        h("div", { class: "modal-body" }, body),
        h("div", { class: "modal-buttons" },
          buttons.map((b) => h("button", { class: b.primary ? "primary" : "", onclick: () => close(b.value) }, b.label)))));
    document.body.append(overlay);
    document.addEventListener("keydown", onKey, true);
    const first = overlay.querySelector<HTMLElement>("input, select, textarea, button.primary");
    first?.focus();
    if (first instanceof HTMLInputElement) first.select();
  });
}

export async function confirmBox(title: string, text: string, ok = "OK"): Promise<boolean> {
  return (await modal(title, h("p", null, text), [{ label: "Cancel", value: "no" }, { label: ok, value: "yes", primary: true }])) === "yes";
}

export async function promptBox(title: string, label: string, value: string): Promise<string | null> {
  const input = h("input", { type: "text", value, class: "wide" });
  const r = await modal(title, h("label", { class: "field" }, h("span", null, label), input),
    [{ label: "Cancel", value: "no" }, { label: "OK", value: "yes", primary: true }]);
  return r === "yes" ? input.value : null;
}
