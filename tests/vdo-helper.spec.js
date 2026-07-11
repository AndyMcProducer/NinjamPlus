const { test, expect } = require("@playwright/test");

const baseParams = {
	room: "cellular-test",
	label: "tester",
	vdoBase: "http://127.0.0.1:8188/vdo-stub",
	chunked: "600",
	chunkbitrate: "600",
	bitrate: "600",
	chunkadaptceil: "900",
};

function helperUrl(overrides = {}) {
	const params = new URLSearchParams({ ...baseParams, ...overrides });
	return "/buffer-room?" + params.toString();
}

async function vdoFrame(page) {
	await expect.poll(() => page.frames().some((frame) => frame.url().includes("/vdo-stub/"))).toBe(true);
	return page.frames().find((frame) => frame.url().includes("/vdo-stub/"));
}

test("uses the weak-uplink profile by default", async ({ page }) => {
	await page.goto(helperUrl({ cameraQuality: "default" }));
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("chunked")).toBe("600");
	expect(params.get("chunkadaptceil")).toBe("900");
	expect(params.get("chunkadaptfloor")).toBe("60");
	expect(params.get("chunkadapt")).toBe("hybrid");
	expect(params.get("chunkadaptresolution")).toBe("1");
	expect(params.get("chunknack")).toBe("1");
});

test("honors an explicit high-quality preset without raising startup bitrate", async ({ page }) => {
	await page.goto(helperUrl({ cameraQuality: "1080p30-10000" }));
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("chunked")).toBe("600");
	expect(params.get("quality")).toBe("0");
	expect(params.get("fps")).toBe("30");
	expect(params.get("maxvideobitrate")).toBe("10000");
	expect(params.get("chunkadaptceil")).toBe("10000");
});

test("updates capture and adaptive encoder limits when quality changes live", async ({ page }) => {
	await page.goto(helperUrl({ cameraQuality: "default" }));
	const frame = await vdoFrame(page);
	await page.selectOption("#vdoCameraQuality", "1080p30-10000");
	await expect.poll(async () => frame.evaluate(() => window.received.length)).toBeGreaterThan(0);
	const messages = await frame.evaluate(() => window.received);
	const liveEval = messages.find((message) => message && message.function === "eval" && String(message.value).includes("applyConstraints"));
	expect(liveEval).toBeTruthy();
	expect(liveEval.value).toContain("chunkadaptceil");
	expect(liveEval.value).toContain("updateVideoProfile");
});

test("reconnects user discovery after a websocket closes", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {
				if (this.onclose) this.onclose();
			}
		};
	});
	await page.goto(helperUrl({ intervalSource: "ws://sync.test/intervals" }));
	await expect.poll(() => page.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);
	const initialCount = await page.evaluate(() => window.createdWebSockets.length);
	await page.evaluate(() => window.createdWebSockets[0].onclose());
	await expect.poll(() => page.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(initialCount);
});
