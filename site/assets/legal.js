// Legal pages carry both languages; show one. Same "rfb-lang" choice as the main site.
(function () {
  const get = () => { try { return localStorage.getItem("rfb-lang"); } catch { return null; } };
  const set = v => { try { localStorage.setItem("rfb-lang", v); } catch {} };
  const labels = { en: { "Legal notice": "Legal notice", "Privacy": "Privacy", "Terms": "Terms", "Back to start": "Back to start" },
                   de: { "Legal notice": "Impressum", "Privacy": "Datenschutz", "Terms": "AGB", "Back to start": "Zur Startseite" } };
  function apply(lang) {
    document.documentElement.lang = lang;
    document.querySelectorAll("[data-lang-block]").forEach(el => { el.hidden = el.dataset.langBlock !== lang; });
    document.querySelectorAll(".lang button").forEach(b => b.setAttribute("aria-pressed", String(b.dataset.lang === lang)));
    document.querySelectorAll("[data-t]").forEach(el => { el.textContent = labels[lang][el.dataset.t]; });
    const h1 = document.querySelector(`[data-lang-block="${lang}"] h1`);
    if (h1) document.title = h1.textContent + " · RESET FPS BOOSTER";
  }
  document.querySelectorAll(".lang button").forEach(b => b.addEventListener("click", () => { set(b.dataset.lang); apply(b.dataset.lang); }));
  apply(get() || ((navigator.language || "").toLowerCase().startsWith("de") ? "de" : "en"));
})();
