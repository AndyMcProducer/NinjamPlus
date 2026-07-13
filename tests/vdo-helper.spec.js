const { test, expect } = require("@playwright/test");

const baseParams = {
	room: "cellular-test",
	label: "tester",
	vdoBase: "http://127.0.0.1:8188/vdo-stub",
	chunked: "2500",
	chunkbitrate: "2500",
	bitrate: "2500",
	chunkadaptceil: "2500",
};

function helperUrl(overrides = {}) {
	const params = new URLSearchParams({ ...baseParams, ...overrides });
	return "/buffer-room?" + params.toString();
}

async function vdoFrame(page) {
	await expect.poll(() => page.frames().some((frame) => frame.url().includes("/vdo-stub/"))).toBe(true);
	return page.frames().find((frame) => frame.url().includes("/vdo-stub/"));
}

test("starts with the quality-first sync-safe profile", async ({ page }) => {
	await page.goto(helperUrl({ cameraQuality: "720p30" }));
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("chunked")).toBe("2500");
	expect(params.get("chunkadaptceil")).toBe("2500");
	expect(params.get("chunkadaptfloor")).toBe("60");
	expect(params.get("chunkadapt")).toBe("bitrate");
	expect(params.get("chunkadaptmaxdrop")).toBe("0");
	expect(params.get("chunkadaptresolution")).toBe("1");
	expect(params.get("chunkedbuffer")).toBe("500");
	expect(params.get("chunkadaptthreshold")).toBe("500");
	expect(params.get("chunkadaptinterval")).toBe("1200");
	expect(params.get("chunkbufferadaptive")).toBe("0");
	expect(params.get("chunknack")).toBe("1");
});

test("honors an explicit high-quality ceiling without lowering startup quality", async ({ page }) => {
	await page.goto(helperUrl({ cameraQuality: "1080p30-10000" }));
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("chunked")).toBe("2500");
	expect(params.get("quality")).toBe("0");
	expect(params.get("fps")).toBe("30");
	expect(params.get("maxvideobitrate")).toBe("10000");
	expect(params.get("chunkadaptceil")).toBe("10000");
});

test("keeps sync-safe adaptation when an operator requests a constrained start", async ({ page }) => {
	await page.goto(helperUrl({
		cameraQuality: "720p30",
		chunked: "300",
		chunkbitrate: "300",
		bitrate: "300",
		chunkadaptceil: "2500",
	}));
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("chunked")).toBe("300");
	expect(params.get("chunkadaptfloor")).toBe("60");
	expect(params.get("chunkadaptceil")).toBe("2500");
	expect(params.get("chunkadapt")).toBe("bitrate");
	expect(params.get("chunkadaptmaxdrop")).toBe("0");
	expect(params.get("chunkadaptresolution")).toBe("1");
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
	expect(liveEval.value).toContain("adaptation.target=cfg.bitrate");

	const applied = await frame.evaluate((source) => {
		window.session = {
			stats: {},
			chunkedRecorder: {
				adaptation: { target: 2500, lastChange: 0, forceKeyFrame: false },
				updateVideoProfile(width, height, ceiling, frameRate) {
					this.appliedProfile = { width, height, ceiling, frameRate };
				},
			},
		};
		window.errorlog = function () {};
		window.eval(source);
		return {
			bitrate: window.session.bitrate,
			chunkbitrate: window.session.chunkbitrate,
			ceiling: window.session.chunkadaptceil,
			target: window.session.chunkedRecorder.adaptation.target,
			statsTarget: window.session.stats.adjustBitrate,
			profile: window.session.chunkedRecorder.appliedProfile,
		};
	}, liveEval.value);
	expect(applied).toEqual({
		bitrate: 10000,
		chunkbitrate: 10000,
		ceiling: 10000,
		target: 10000,
		statsTarget: 10000,
		profile: { width: 1920, height: 1080, ceiling: 10000, frameRate: 30 },
	});
});

test("applies the exact NINJAM buffer and compensates measured video pipeline changes", async ({ page }) => {
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
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);

	await frame.evaluate(() => {
		parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*");
	});
	await appFrame.evaluate(() => {
		window.createdWebSockets[0].onmessage({
			data: JSON.stringify({
				type: "videoTimecode",
				userId: "tester",
				interval: 42,
				timecode: 0,
				bufferCalculated: true,
				receiverBufferMs: 15962,
				receiverBufferFinal: true,
				serverRouteLatencyReady: true,
				serverRouteLatencyMs: 218,
			}),
		});
	});
	await frame.evaluate(() => {
		parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*");
	});
	await expect.poll(async () => frame.evaluate(() => {
		const updates = window.received.filter((message) => message && message.setBufferDelay === 15962);
		return updates.length;
	})).toBeGreaterThan(0);

	await frame.evaluate(() => {
		parent.postMessage({ streamID: "remote_stream", label: "tester", cameraSendLatencyMs: 218 }, "*");
	});
	await expect.poll(async () => frame.evaluate(() => {
		const updates = window.received.filter((message) => message && message.setBufferDelay === 15744);
		return updates.length;
	})).toBeGreaterThan(0);

	await frame.evaluate(() => {
		parent.postMessage({ streamID: "remote_stream", label: "tester", cameraSendLatencyMs: 900 }, "*");
	});
	await expect.poll(async () => frame.evaluate(() => {
		const updates = window.received.filter((message) => message && message.setBufferDelay === 15694);
		return updates.length;
	})).toBeGreaterThan(0);
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
