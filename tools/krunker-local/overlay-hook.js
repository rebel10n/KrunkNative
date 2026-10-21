// Harness-only trace, injected before the page's scripts on the loopback page (launch.mjs, Page.addScriptToEvaluateOnNewDocument).
// It records the first writes of the disconnect overlay: the innerHTML of #instructionsUpdate, and the classList.add of
// "onError" on #instructionHolder. Each record is a console line "[ovl] ..." with the element, the value (cut to 400
// characters) and the stack, so the bundle function that writes the overlay can be named. It changes nothing else.
(() => {
  const seen = { html: 0, onError: 0 };
  const stackOf = () => String(new Error().stack).split("\n").slice(2, 14).map((l) => l.trim().slice(0, 140)).join(" | ");
  const desc = Object.getOwnPropertyDescriptor(Element.prototype, "innerHTML");
  Object.defineProperty(Element.prototype, "innerHTML", {
    configurable: true,
    get() { return desc.get.call(this); },
    set(v) {
      if (this.id === "instructionsUpdate" && seen.html < 4) {
        seen.html++;
        console.log("[ovl] innerHTML #" + this.id + " = " + String(v).slice(0, 400) + " @ " + stackOf());
      }
      return desc.set.call(this, v);
    },
  });
  // classList is a getter that returns a DOMTokenList; remember the element each list belongs to.
  const listOwner = new WeakMap();
  const clDesc = Object.getOwnPropertyDescriptor(Element.prototype, "classList");
  Object.defineProperty(Element.prototype, "classList", {
    configurable: true,
    get() {
      const list = clDesc.get.call(this);
      listOwner.set(list, this);
      return list;
    },
  });
  const addNative = DOMTokenList.prototype.add;
  DOMTokenList.prototype.add = function (...tokens) {
    const el = listOwner.get(this);
    if (el && el.id === "instructionHolder" && tokens.indexOf("onError") >= 0 && seen.onError < 4) {
      seen.onError++;
      console.log("[ovl] classList.add onError on #" + el.id + " (classes before: " + el.className + ") @ " + stackOf());
    }
    return addNative.apply(this, tokens);
  };
})();
