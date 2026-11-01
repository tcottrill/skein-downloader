const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const root = path.resolve(__dirname, "..");
const source = fs.readFileSync(path.join(root, "website-localization.js"), "utf8");
const html = fs.readFileSync(path.join(root, "index.html"), "utf8");

function createHarness(saved = null, preferences = ["en-US"]) {
  const instrumented = source.replace(
    "  const renderMenu = activeCode => {",
    `  globalThis.__test = { languages, messages, getSavedLanguage, browserLanguage, getSelected: () => selected, resolve: resolved, select: value => { selected = value; } };\n  return;\n  const renderMenu = activeCode => {`
  );
  assert.notEqual(instrumented, source, "test hook should match the localization script");
  const sandbox = {
    navigator: { languages: preferences, language: preferences[0] || "en" },
    localStorage: { getItem: () => saved },
    document: { getElementById: () => ({}) }
  };
  vm.runInNewContext(instrumented, sandbox, { filename: "website-localization.js" });
  return sandbox.__test;
}

function createBrowserHarness(saved = null, preferences = ["en-US"]) {
  const savedValues = new Map(saved === null ? [] : [["skein-site-language", saved]]);
  const idElements = new Map();
  const textElements = [];
  const attributeElements = [];
  const documentListeners = new Map();
  const makeElement = (dataset = {}) => ({
    dataset,
    innerHTML: "",
    textContent: "",
    attributes: {},
    setAttribute(name, value) { this.attributes[name] = value; },
    focus() { this.focused = true; }
  });
  const flag = makeElement();
  flag.outerHTML = '<span class="flag" id="currentFlag"></span>';
  Object.defineProperty(flag, "outerHTML", {
    get() { return this.markup; },
    set(markup) {
      const next = makeElement();
      next.markup = markup;
      Object.defineProperty(next, "outerHTML", {
        get() { return this.markup; },
        set(value) {
          const replacement = makeElement();
          replacement.markup = value;
          Object.defineProperty(replacement, "outerHTML", Object.getOwnPropertyDescriptor(this, "outerHTML"));
          idElements.set("currentFlag", replacement);
        }
      });
      idElements.set("currentFlag", next);
    }
  });
  const picker = makeElement();
  picker.open = false;
  picker.contains = target => target === menu || target === picker;
  const summary = makeElement();
  picker.querySelector = selector => selector === "summary" ? summary : null;
  const menu = makeElement();
  menu.buttons = [];
  menu.addEventListener = (type, handler) => { menu.handler = type === "click" ? handler : menu.handler; };
  Object.defineProperty(menu, "innerHTML", {
    get() { return this.markup || ""; },
    set(markup) {
      this.markup = markup;
      this.buttons = [...markup.matchAll(/<button[^>]*data-language="([^"]+)"[^>]*>/g)].map(match => {
        const button = makeElement({ language: match[1] });
        button.closest = selector => selector === "button[data-language]" ? button : null;
        button.ariaCurrent = /aria-current="true"/.test(match[0]);
        return button;
      });
    }
  });
  idElements.set("languagePicker", picker);
  idElements.set("languageMenu", menu);
  idElements.set("currentLanguage", makeElement());
  idElements.set("currentFlag", flag);

  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bdata-i18n="([^"]+)"[^>]*)>/gi)) {
    textElements.push(makeElement({ i18n: match[3] }));
  }
  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bdata-i18n-attr="([^"]+)"[^>]*)>/gi)) {
    attributeElements.push(makeElement({ i18nAttr: match[3] }));
  }
  const meta = makeElement();
  const document = {
    documentElement: { lang: "" },
    title: "",
    getElementById: id => idElements.get(id) || null,
    querySelector: selector => selector === 'meta[name="description"]' ? meta : null,
    querySelectorAll: selector => selector === "[data-i18n]" ? textElements : selector === "[data-i18n-attr]" ? attributeElements : [],
    addEventListener(type, handler) { documentListeners.set(type, handler); }
  };
  const navigator = { languages: preferences, language: preferences[0] || "en" };
  const localStorage = {
    getItem: key => savedValues.get(key) ?? null,
    setItem: (key, value) => savedValues.set(key, value)
  };
  const sandbox = { document, navigator, localStorage };
  vm.runInNewContext(source, sandbox, { filename: "website-localization.js" });
  return { document, navigator, localStorage, idElements, menu, picker, summary, meta, textElements, attributeElements, documentListeners };
}

test("every marked page string and attribute has a value in all thirteen locales", () => {
  const catalog = createHarness();
  const htmlKeys = new Set([...html.matchAll(/data-i18n="([^"]+)"/g)].map(match => match[1]));
  for (const match of html.matchAll(/data-i18n-attr="([^"]+)"/g)) {
    for (const pair of match[1].split(",")) htmlKeys.add(pair.split(":")[1]);
  }
  htmlKeys.add("pageTitle");
  htmlKeys.add("metaDescription");
  htmlKeys.add("sameAsSystem");
  assert.equal(catalog.languages.length, 13);
  for (const language of catalog.languages) {
    const actualKeys = Object.keys(catalog.messages[language.code]);
    assert.deepEqual(new Set(actualKeys), htmlKeys, `${language.code} catalog keys`);
  }
});

test("system language matching recognizes Chinese script and regional variants", () => {
  for (const tag of ["zh-TW", "zh-Hant", "zh-HK", "zh-MO", "zh-Hant-HK"]) {
    assert.equal(createHarness(null, [tag]).browserLanguage(), "zh_TW", tag);
  }
  for (const tag of ["zh", "zh-CN", "zh-Hans", "zh-SG", "zh-Hans-HK"]) {
    assert.equal(createHarness(null, [tag]).browserLanguage(), "zh_CN", tag);
  }
  assert.equal(createHarness(null, ["fr-CA"]).browserLanguage(), "fr");
  assert.equal(createHarness(null, ["pt-PT"]).browserLanguage(), "pt");
  assert.equal(createHarness(null, ["pt-BR"]).browserLanguage(), "pt");
  assert.equal(createHarness(null, ["pt-AO", "hi-IN"]).browserLanguage(), "pt");
  assert.equal(createHarness(null, ["hi-IN"]).browserLanguage(), "hi");
  assert.equal(createHarness(null, ["xx-YY"]).browserLanguage(), "en");
  assert.equal(createHarness(null, ["xx-YY", "ja-JP"]).browserLanguage(), "ja");
});

test("a saved explicit language, including English, overrides system detection", () => {
  const explicit = createHarness("de", ["ja-JP"]);
  assert.equal(explicit.getSelected(), "de");
  assert.equal(explicit.resolve(), "de");

  const explicitEnglish = createHarness("en", ["zh-TW"]);
  assert.equal(explicitEnglish.resolve(), "en");

  const automatic = createHarness("system", ["zh-HK"]);
  assert.equal(automatic.resolve(), "zh_TW");

  const unsupportedStoredValue = createHarness("<img src=x>", ["vi-VN"]);
  assert.equal(unsupportedStoredValue.getSavedLanguage(), null);
  assert.equal(unsupportedStoredValue.getSelected(), "system");
  assert.equal(unsupportedStoredValue.resolve(), "vi");
});

test("localized rich text keeps the existing release, issue, and Ko-fi destinations", () => {
  const catalog = createHarness();
  const expected = {
    unsignedNotice: ["https://github.com/tcottrill/skein-downloader#downloads"],
    requestText: ["https://github.com/tcottrill/skein-downloader/issues", "https://ko-fi.com/timcottrill"]
  };
  for (const language of catalog.languages) {
    for (const [key, urls] of Object.entries(expected)) {
      const actualUrls = [...catalog.messages[language.code][key].matchAll(/href=\\?"([^"\\]+)\\?"/g)].map(match => match[1]);
      assert.deepEqual(actualUrls, urls, `${language.code}.${key} links`);
    }
  }
});

test("menu selection updates the page, persists across reload, and system reset restores detection", () => {
  const first = createBrowserHarness(null, ["ja-JP"]);
  assert.equal(first.document.documentElement.lang, "ja");
  assert.equal(first.idElements.get("currentLanguage").textContent, "日本語");
  const englishFlag = first.idElements.get("currentFlag").outerHTML;

  first.picker.open = true;
  first.menu.handler({ target: first.menu.buttons.find(button => button.dataset.language === "fr") });
  assert.equal(first.localStorage.getItem("skein-site-language"), "fr");
  assert.equal(first.picker.open, false);
  assert.equal(first.summary.focused, true);
  assert.equal(first.document.documentElement.lang, "fr");
  assert.equal(first.document.title, "Skein Downloader — plusieurs connexions, un seul fichier.");
  assert.match(first.meta.content, /gestionnaire de téléchargements natif/);
  assert.equal(first.idElements.get("currentLanguage").textContent, "Français");
  assert.notEqual(first.idElements.get("currentFlag").outerHTML, englishFlag);
  assert.equal(first.textElements.find(element => element.dataset.i18n === "navDownloads").innerHTML, "Téléchargements");

  const reloaded = createBrowserHarness(first.localStorage.getItem("skein-site-language"), ["ko-KR"]);
  assert.equal(reloaded.document.documentElement.lang, "fr");
  assert.equal(reloaded.idElements.get("currentLanguage").textContent, "Français");
  reloaded.picker.open = true;
  reloaded.menu.handler({ target: reloaded.menu.buttons.find(button => button.dataset.language === "system") });
  assert.equal(reloaded.localStorage.getItem("skein-site-language"), "system");
  assert.equal(reloaded.document.documentElement.lang, "ko");
  assert.equal(reloaded.idElements.get("currentLanguage").textContent, "한국어");

  reloaded.picker.open = true;
  reloaded.menu.handler({ target: reloaded.menu.buttons.find(button => button.dataset.language === "pt") });
  assert.equal(reloaded.localStorage.getItem("skein-site-language"), "pt");
  assert.equal(reloaded.document.documentElement.lang, "pt");
  assert.equal(reloaded.idElements.get("currentLanguage").textContent, "Português");
  const portugueseReload = createBrowserHarness(reloaded.localStorage.getItem("skein-site-language"), ["hi-IN"]);
  assert.equal(portugueseReload.document.documentElement.lang, "pt");
  assert.equal(portugueseReload.idElements.get("currentLanguage").textContent, "Português");

  portugueseReload.menu.handler({ target: portugueseReload.menu.buttons.find(button => button.dataset.language === "hi") });
  assert.equal(portugueseReload.localStorage.getItem("skein-site-language"), "hi");
  const hindiReload = createBrowserHarness(portugueseReload.localStorage.getItem("skein-site-language"), ["pt-BR"]);
  assert.equal(hindiReload.document.documentElement.lang, "hi");
  assert.equal(hindiReload.idElements.get("currentLanguage").textContent, "हिन्दी");

  reloaded.picker.open = true;
  reloaded.documentListeners.get("keydown")({ key: "Escape" });
  assert.equal(reloaded.picker.open, false);
  assert.equal(reloaded.summary.focused, true);
});
