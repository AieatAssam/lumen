/**
 * Lumen E2E browser tests
 *
 * Run: node tests/e2e.spec.mjs [url]
 * Default URL: https://aieatassam.github.io/lumen/
 */

import { chromium } from "playwright-core";

const URL = process.argv[2] || "https://aieatassam.github.io/lumen/";

let passed = 0;
let failed = 0;

function ok(name, condition, detail = "") {
  if (condition) {
    console.log(`  ✅ ${name}`);
    passed++;
  } else {
    console.log(`  ❌ ${name}${detail ? " — " + detail : ""}`);
    failed++;
  }
}

async function waitForPixels(page, ms = 4000) {
  await page.waitForTimeout(ms);
  return page.evaluate(() => {
    const c = document.querySelector("canvas");
    if (!c) return null;
    const ctx = c.getContext("2d");
    const d = ctx.getImageData(320, 180, 1, 1).data;
    return Array.from(d);
  });
}

// Use evaluate-based click — Playwright locator clicks land on SVG icons
// inside React buttons without triggering React synthetic event handlers.
async function clickBtn(page, text) {
  await page.evaluate((t) => {
    const btn = [...document.querySelectorAll("button")]
      .find(b => b.textContent.trim() === t);
    if (btn) btn.click();
  }, text);
}

async function btnCount(page, text) {
  return page.evaluate((t) => {
    return [...document.querySelectorAll("button")]
      .filter(b => b.textContent.trim() === t).length;
  }, text);
}

async function main() {
  console.log(`\n🧪 Lumen E2E Tests — ${URL}\n`);

  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });

  const errors = [];
  page.on("console", (msg) => {
    if (msg.type() === "error") errors.push(msg.text());
  });
  page.on("pageerror", (err) => errors.push("PAGE: " + err.message));

  try {
    // ── 1. Page load ──
    console.log("📄 Page load");
    await page.goto(URL, { waitUntil: "networkidle", timeout: 30000 });
    ok("page loads without JS errors", errors.length === 0, errors.join("; "));

    const title = await page.title();
    ok("page title is Lumen", title.includes("Lumen"), title);

    // ── 2. Scene buttons exist ──
    console.log("\n🎬 Scene buttons");
    const sceneNames = ["Cornell Box", "Metal Spheres", "Glass & Light",
      "Random Spheres", "Checkerboard", "Cosmic"];
    for (const name of sceneNames) {
      ok(`"${name}" button present`, (await btnCount(page, name)) > 0);
    }

    // ── 3. WASM ready ──
    console.log("\n⚡ WASM initialization");
    // Wait for "Loading WASM engine…" to disappear
    const wasmReady = await page.evaluate(() => {
      return new Promise((resolve) => {
        const start = Date.now();
        const check = () => {
          if (!document.body.textContent.includes("Loading WASM engine")) {
            resolve(true);
          } else if (Date.now() - start > 20000) {
            resolve(false);
          } else {
            setTimeout(check, 500);
          }
        };
        check();
      });
    });
    ok("WASM loaded (no loading text)", wasmReady);

    // ── 4. Render ──
    console.log("\n🎨 Render");
    ok("Render button visible", (await btnCount(page, "Render")) > 0);
    await clickBtn(page, "Render");
    await page.waitForTimeout(1500);

    ok("button changed to Pause", (await btnCount(page, "Pause")) > 0);

    // Verify scene buttons disabled
    const cornellDisabled = await page.evaluate(() => {
      const b = [...document.querySelectorAll("button")]
        .find(b => b.textContent.trim() === "Cornell Box");
      return b ? b.disabled : null;
    });
    ok("scene buttons disabled during render", cornellDisabled === true);

    // ── 5. Pixel output ──
    console.log("\n🖼️  Pixel output");
    const pixels = await waitForPixels(page, 5000);
    ok("canvas has pixel data", pixels !== null && pixels.length === 4, String(pixels));
    if (pixels && pixels.length === 4) {
      const hasColor = pixels[0] > 0 || pixels[1] > 0 || pixels[2] > 0;
      ok("pixels are non-black (rendering works)", hasColor, JSON.stringify(pixels));
    }

    // ── 6. Pause ──
    console.log("\n⏸️  Pause");
    const samplesBeforePause = await page.evaluate(() => {
      const body = document.body.textContent;
      const m = body.match(/(\d+)\s*spp/i);
      return m ? parseInt(m[1]) : -1;
    });
    ok("samples counter present", samplesBeforePause > 0, String(samplesBeforePause));

    await clickBtn(page, "Pause");
    await page.waitForTimeout(1000);
    ok("button changed back to Render", (await btnCount(page, "Render")) > 0);

    // Wait and verify samples don't increase (rendering stopped)
    await page.waitForTimeout(3000);
    const samplesAfterPause = await page.evaluate(() => {
      const body = document.body.textContent;
      const m = body.match(/(\d+)\s*spp/i);
      return m ? parseInt(m[1]) : -1;
    });
    ok("samples stopped accumulating (render paused)",
      Math.abs(samplesAfterPause - samplesBeforePause) <= 1,
      `before=${samplesBeforePause} after=${samplesAfterPause}`);

    // ── 7. Scene change ──
    console.log("\n🔄 Scene change");
    await clickBtn(page, "Checkerboard");
    await page.waitForTimeout(500);
    ok("Checkerboard scene selectable", (await btnCount(page, "Checkerboard")) > 0);

    // ── 8. Camera controls ──
    console.log("\n📷 Camera controls");
    await page.evaluate(() => {
      const btn = [...document.querySelectorAll("button")]
        .find(b => b.getAttribute("title") === "Pan right");
      if (btn) btn.click();
    });
    await page.waitForTimeout(500);
    const yawText = await page.evaluate(() => document.body.textContent);
    ok("camera controls present", yawText.includes("Yaw:"));

    // ── 9. Render new scene ──
    console.log("\n🎨 Render (Checkerboard)");
    await clickBtn(page, "Render");
    await page.waitForTimeout(1000);
    // Verify render started
    const started = (await btnCount(page, "Pause")) > 0;
    ok("render started (button shows Pause)", started);
    await page.waitForTimeout(6000);

    const newPixels = await page.evaluate(() => {
      const c = document.querySelector("canvas");
      if (!c) return null;
      const ctx = c.getContext("2d");
      return Array.from(ctx.getImageData(320, 180, 1, 1).data);
    });
    const hasColor = newPixels && (newPixels[0] > 0 || newPixels[1] > 0 || newPixels[2] > 0);
    ok("checkerboard produces real pixels", hasColor, JSON.stringify(newPixels));

    // ── 10. Screenshot ──
    console.log("\n📸 Screenshot");
    await page.screenshot({ path: "/tmp/lumen-e2e-final.png", fullPage: false });
    ok("screenshot saved", true);

    // ── 11. Final JS errors ──
    console.log("\n🐛 JS Errors");
    ok("no uncaught JavaScript errors", errors.length === 0, errors.join("; "));

  } catch (err) {
    console.error("FATAL:", err.message);
    failed++;
  } finally {
    await browser.close();
  }

  console.log(`\n${"═".repeat(50)}`);
  console.log(`  ${passed} passed, ${failed} failed, ${passed + failed} total`);
  console.log(`${"═".repeat(50)}\n`);
  process.exit(failed > 0 ? 1 : 0);
}

main();
