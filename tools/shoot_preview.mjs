/*
 * Screenshot the HTML rendering of the manual for a visual check.
 *
 * The HTML is produced by tools/md_to_docx.py from the same block model that
 * feeds the DOCX, so this verifies content and styling intent. It cannot verify
 * Word's own pagination (no LibreOffice/Word in this environment).
 *
 * Usage: node tools/shoot_preview.mjs build/manual-preview.html build/shots
 */
import { createRequire } from "node:module";
import { mkdirSync } from "node:fs";
import { resolve } from "node:path";
import process from "node:process";

const require = createRequire(import.meta.url);
const playwrightPath = (process.env.PW_MODULE ?? "playwright").trim();
const { chromium } = require(playwrightPath);

const [input, outDir] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });

const page_path = resolve(input);
const browser = await chromium.launch();
const page = await browser.newPage({
  viewport: { width: 980, height: 1320 },
  deviceScaleFactor: 1,
});
await page.goto("file:///" + page_path.replace(/\\/g, "/"));

const shots = [
  ["01-title", null, 0],
  ["02-quickstart", "h3", "设备端最小程序"],
  ["03-table", "h2", "7. 常见问题与排错"],
  ["04-appendix", "h2", "附录 B · 错误码全表"],
];

for (const [name, selector, needle] of shots) {
  if (selector) {
    const target = page.locator(`${selector}:has-text("${needle}")`).first();
    await target.scrollIntoViewIfNeeded();
    await page.waitForTimeout(120);
  } else {
    await page.evaluate(() => window.scrollTo(0, 0));
  }
  await page.screenshot({ path: `${outDir}/${name}.png` });
  console.log("shot", name);
}

const height = await page.evaluate(() => document.body.scrollHeight);
console.log("page height", height);
await browser.close();
