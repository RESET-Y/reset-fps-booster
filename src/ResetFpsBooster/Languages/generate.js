// Writes Strings.{en,de,ru}.xaml from strings.json.
//
//   node Languages/generate.js
//
// strings.json is the only place a text is edited. This refuses to write
// anything if an entry lacks one of the languages, so a language can never
// quietly show a key where the others show a sentence.

const fs = require("fs");
const path = require("path");

const LANGS = ["en", "de", "ru"];
const dir = __dirname;
const table = JSON.parse(fs.readFileSync(path.join(dir, "strings.json"), "utf8"));

const missing = [];
for (const [key, entry] of Object.entries(table)) {
    if (key.startsWith("_")) continue;
    for (const lang of LANGS) {
        if (typeof entry[lang] !== "string") missing.push(`${key} [${lang}]`);
    }
}
if (missing.length) {
    console.error("Missing translations - nothing written:\n  " + missing.join("\n  "));
    process.exit(1);
}

const esc = (s) => s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");

for (const lang of LANGS) {
    const lines = [
        "<!-- GENERATED from strings.json by generate.js. Do not edit by hand. -->",
        "<ResourceDictionary xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"",
        "                    xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"",
        "                    xmlns:sys=\"clr-namespace:System;assembly=mscorlib\">",
    ];
    for (const [key, entry] of Object.entries(table)) {
        if (key.startsWith("_")) continue;
        // xml:space keeps leading/trailing blanks, which prefixes like
        // "Current status: " depend on.
        lines.push(`    <sys:String x:Key="${esc(key)}" xml:space="preserve">${esc(entry[lang])}</sys:String>`);
    }
    lines.push("</ResourceDictionary>", "");
    fs.writeFileSync(path.join(dir, `Strings.${lang}.xaml`), lines.join("\n"), "utf8");
}

const count = Object.keys(table).filter((k) => !k.startsWith("_")).length;
console.log(`${count} texts written for ${LANGS.join(", ")}.`);
