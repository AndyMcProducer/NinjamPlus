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
	await expect(page.locator("#vdoCameraQuality")).toHaveValue("720p30");
	const frame = await vdoFrame(page);
	const params = new URL(frame.url()).searchParams;
	expect(params.get("quality")).toBe("1");
	expect(params.get("mfr")).toBe("30");
	expect(params.has("fps")).toBe(false);
	expect(params.get("maxvideobitrate")).toBe("2500");
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
	expect(params.get("mfr")).toBe("30");
	expect(params.has("fps")).toBe(false);
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
	const initialUpdates = await frame.evaluate(() => window.received.filter((message) => message && message.setBufferDelay === 15962));
	expect(initialUpdates.every((message) => message.streamID === "remote_stream")).toBe(true);
	expect(initialUpdates.some((message) => "UUID" in message || "target" in message)).toBe(false);

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

test("does not reapply a stable buffer for every interval or duplicate refresh", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {}
		};
	});
	await page.goto(helperUrl({ intervalSource: "ws://sync.test/intervals" }));
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);

	const sendInterval = (interval, extra = {}) => appFrame.evaluate(({ interval, extra }) => {
		window.createdWebSockets[0].onmessage({
			data: JSON.stringify({
				type: "videoTimecode",
				userId: "tester",
				interval,
				timecode: 0,
				bufferCalculated: true,
				receiverBufferMs: 800,
				receiverBufferFinal: true,
				...extra,
			}),
		});
	}, { interval, extra });

	await sendInterval(1);
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*"));
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 800).length)).toBeGreaterThan(0);
	await frame.evaluate(() => { window.received = []; });

	for (let interval = 2; interval <= 13; interval += 1) {
		await sendInterval(interval);
	}
	await page.waitForTimeout(500);
	expect(await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m))).toEqual([]);

	// A transient payload without buffer state must not erase the last value or
	// cause a fallback/default command.
	await sendInterval(14, { receiverBufferMs: undefined });
	await page.waitForTimeout(500);
	expect(await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m))).toEqual([]);

	await sendInterval(15, { refreshBuffer: true, bufferRefreshEventId: "refresh:tester:1" });
	await page.waitForTimeout(250);
	await frame.evaluate(() => { window.received = []; });
	await sendInterval(15, { refreshBuffer: true, bufferRefreshEventId: "refresh:tester:1" });
	await page.waitForTimeout(500);
	expect(await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m))).toEqual([]);
});

test("recovers stable buffer state after a real HTTP-polling helper reload", async ({ page }) => {
	let interval = 1;
	await page.route("**/snapshot-intervals", async (route) => {
		await route.fulfill({
			status: 200,
			contentType: "application/json",
			body: JSON.stringify([{
				type: "videoTimecode",
				userId: "tester",
				interval: interval++,
				timecode: 0,
				bufferCalculated: true,
				receiverBufferMs: 800,
				receiverBufferFinal: true,
			}]),
		});
	});

	await page.goto(helperUrl({
		intervalSource: "http://127.0.0.1:8188/snapshot-intervals",
		intervalPollMs: "100",
	}));
	let frame = await vdoFrame(page);
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*"));
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 800).length)).toBeGreaterThan(0);
	await frame.evaluate(() => { window.received = []; });
	await page.waitForTimeout(500);
	expect(await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m))).toEqual([]);

	await page.reload();
	frame = await vdoFrame(page);
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*"));
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 800).length)).toBeGreaterThan(0);
});

test("uses only uncontrollable WebRTC encode and decode latency", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {}
		};
	});
	await page.goto(helperUrl({ intervalSource: "ws://sync.test/intervals" }));
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);
	await appFrame.evaluate(() => {
		window.createdWebSockets[0].onmessage({
			data: JSON.stringify({
				type: "videoTimecode",
				userId: "tester",
				interval: 1,
				timecode: 0,
				bufferCalculated: true,
				receiverBufferMs: 1000,
				receiverBufferFinal: true,
			}),
		});
	});
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*"));
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 1000).length)).toBeGreaterThan(0);
	await frame.evaluate(() => { window.received = []; });

	await frame.evaluate(() => {
		parent.postMessage({
			streamID: "remote_stream",
			label: "tester",
			totalEncodeTime: 0.2,
			framesEncoded: 10,
			totalDecodeTime: 0.3,
			totalProcessingDelay: 0.5,
			framesDecoded: 10,
		}, "*");
	});
	// totalProcessingDelay spans first-packet receipt through decode and therefore
	// includes the receiver jitter buffer controlled by setBufferDelay. It must not
	// replace or be added to the decoder-only measurement.
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 950).length)).toBeGreaterThan(0);
	expect(await frame.evaluate(() => window.received.some((m) => m && (m.setBufferDelay === 930 || m.setBufferDelay === 900)))).toBe(false);
});

test("maps VDO quick-stat stream keys to decoder-only peer latency", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {}
		};
	});
	await page.goto(helperUrl({ intervalSource: "ws://sync.test/intervals" }));
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);
	await appFrame.evaluate(() => {
		window.createdWebSockets[0].onmessage({
			data: JSON.stringify({
				type: "videoTimecode",
				userId: "tester",
				interval: 1,
				timecode: 0,
				bufferCalculated: true,
				receiverBufferMs: 1000,
				receiverBufferFinal: true,
			}),
		});
	});
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "tester" } }, "*"));
	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 1000).length)).toBeGreaterThan(0);
	await frame.evaluate(() => { window.received = []; });

	await frame.evaluate(() => {
		parent.postMessage({
			stats: {
				streamID: "local_stream",
				inbound: {
					remote_stream: {
						chunked_mode_video: {
							decodeLatencyMs: 25,
							Jitter_Buffer_ms: 400,
							Added_Buffer_Delay_ms: 350,
							Total_Playout_Delay_ms: 800,
						},
					},
				},
			},
		}, "*");
	});

	await expect.poll(() => frame.evaluate(() => window.received.filter((m) => m && m.setBufferDelay === 975).length)).toBeGreaterThan(0);
	expect(await frame.evaluate(() => window.received.some((m) => m && (m.setBufferDelay === 600 || m.setBufferDelay === 200)))).toBe(false);

	await frame.evaluate(() => { window.received = []; });
	await frame.evaluate(() => {
		parent.postMessage({
			stats: {
				streamID: "local_stream",
				inbound: {
					remote_stream: { chunked_mode_video: { decodeLatencyMs: 28 } },
				},
			},
		}, "*");
	});
	await page.waitForTimeout(350);
	expect(await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m))).toEqual([]);

	await frame.evaluate(() => {
		[80, 130, 180].forEach((decodeLatencyMs) => {
			parent.postMessage({
				stats: {
					streamID: "local_stream",
					inbound: {
						remote_stream: { chunked_mode_video: { decodeLatencyMs } },
					},
				},
			}, "*");
		});
	});
	await page.waitForTimeout(500);
	const debouncedUpdates = await frame.evaluate(() => window.received.filter((m) => m && "setBufferDelay" in m));
	expect(debouncedUpdates).toHaveLength(1);
	expect(debouncedUpdates[0].setBufferDelay).toBe(888);
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

test("broadcasts each native interval marker once over the VDO peer data channel", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {}
		};
	});
	await page.goto(helperUrl({
		label: "local-user",
		vdoSyncUserKey: "local-user",
		intervalSource: "ws://sync.test/intervals",
	}));
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);
	await frame.evaluate(() => { window.received = []; });

	const marker = {
		type: "vdoPeerSyncTag",
		signalType: "intervalSyncTag",
		queuedAtWallClockMs: Date.now(),
		payload: {
			type: "intervalSyncTag",
			userId: "local-user",
			syncSessionId: "session-local",
			intervalIndex: 4,
			intervalAbsolute: 12,
			bpi: 16,
			beatIndex: 0,
			sendOffsetMs: 7,
			eventId: "intervalTag:local-user:1",
		},
	};
	await appFrame.evaluate((payload) => {
		window.createdWebSockets[0].onmessage({ data: JSON.stringify(payload) });
	}, marker);
	await expect.poll(() => frame.evaluate(() => window.received.filter((message) => message?.sendData?.ninjamPlusSync).length)).toBe(1);

	const sent = await frame.evaluate(() => window.received.find((message) => message?.sendData?.ninjamPlusSync));
	expect(sent.type).toBe("pcs");
	expect(sent.sendData.ninjamPlusSync.version).toBe(1);
	expect(sent.sendData.ninjamPlusSync.signalType).toBe("intervalSyncTag");
	expect(sent.sendData.ninjamPlusSync.payload.eventId).toBe(marker.payload.eventId);
	expect(sent.sendData.ninjamPlusSync.payload.sendOffsetMs).toBeGreaterThanOrEqual(marker.payload.sendOffsetMs);

	await appFrame.evaluate((payload) => {
		window.createdWebSockets[0].onmessage({ data: JSON.stringify(payload) });
	}, marker);
	await page.waitForTimeout(200);
	expect(await frame.evaluate(() => window.received.filter((message) => message?.sendData?.ninjamPlusSync).length)).toBe(1);
});

test("forwards received peer markers and targeted acknowledgements to native", async ({ page }) => {
	const forwarded = [];
	await page.route("**/vdo-peer-sync?**", async (route) => {
		forwarded.push(JSON.parse(route.request().postData()));
		await route.fulfill({ status: 204, body: "" });
	});
	await page.goto(helperUrl({ label: "Local User", vdoSyncUserKey: "local user" }));
	const frame = await vdoFrame(page);
	await frame.evaluate(() => {
		window.received = [];
		parent.postMessage({
			action: "guest-connected",
			streamID: "remote-stream",
			UUID: "remote-peer-uuid",
			value: { label: "Remote User" },
		}, "*");
	});

	const tagEnvelope = {
		version: 1,
		signalType: "intervalSyncTag",
		payload: {
			type: "intervalSyncTag",
			userId: "remote user",
			syncSessionId: "session-remote",
			intervalIndex: 4,
			intervalAbsolute: 9,
			bpi: 16,
			beatIndex: 0,
			sendOffsetMs: 10,
			eventId: "intervalTag:remote user:7",
		},
	};
	await frame.evaluate((ninjamPlusSync) => {
		parent.postMessage({ dataReceived: { ninjamPlusSync }, UUID: "remote-peer-uuid" }, "*");
	}, tagEnvelope);
	await expect.poll(() => forwarded.length).toBe(1);

	expect(forwarded[0].version).toBe(1);
	expect(forwarded[0].signalType).toBe("intervalSyncTag");
	expect(forwarded[0].sender).toBe("remote user");
	expect(forwarded[0].payload.vdoPeerReceivedWallClockMs).toEqual(expect.any(Number));
	const ack = await frame.evaluate(() => window.received.find((message) => message?.sendData?.ninjamPlusSync?.signalType === "intervalSyncAck"));
	expect(ack.type).toBe("pcs");
	expect(ack.UUID).toBe("remote-peer-uuid");
	expect(ack.sendData.ninjamPlusSync.payload.userId).toBe("local user");
	expect(ack.sendData.ninjamPlusSync.payload.targetUserId).toBe("remote user");
	expect(ack.sendData.ninjamPlusSync.payload.ackEventId).toBe(tagEnvelope.payload.eventId);

	await frame.evaluate((ninjamPlusSync) => {
		parent.postMessage({ dataReceived: { ninjamPlusSync }, UUID: "remote-peer-uuid" }, "*");
	}, tagEnvelope);
	await page.waitForTimeout(200);
	expect(forwarded).toHaveLength(1);

	const spoofedTag = structuredClone(tagEnvelope);
	spoofedTag.payload.userId = "another user";
	spoofedTag.payload.eventId = "intervalTag:another user:8";
	await frame.evaluate((ninjamPlusSync) => {
		parent.postMessage({ dataReceived: { ninjamPlusSync }, UUID: "remote-peer-uuid" }, "*");
	}, spoofedTag);
	await page.waitForTimeout(200);
	expect(forwarded).toHaveLength(1);

	const wrongTargetAck = {
		version: 1,
		signalType: "intervalSyncAck",
		payload: {
			type: "intervalSyncAck",
			userId: "remote user",
			targetUserId: "another user",
			ackEventId: "intervalTag:local user:3",
			eventId: "vdoPeerAck:remote user:3",
		},
	};
	await frame.evaluate((ninjamPlusSync) => {
		parent.postMessage({ dataReceived: { ninjamPlusSync }, UUID: "remote-peer-uuid" }, "*");
	}, wrongTargetAck);
	await page.waitForTimeout(200);
	expect(forwarded).toHaveLength(1);

	const targetedAck = structuredClone(wrongTargetAck);
	targetedAck.payload.targetUserId = "local user";
	await frame.evaluate((ninjamPlusSync) => {
		parent.postMessage({ dataReceived: { ninjamPlusSync }, UUID: "remote-peer-uuid" }, "*");
	}, targetedAck);
	await expect.poll(() => forwarded.length).toBe(2);
	expect(forwarded[1].version).toBe(1);
	expect(forwarded[1].signalType).toBe("intervalSyncAck");
	expect(forwarded[1].sender).toBe("remote user");
	expect(forwarded[1].payload.targetUserId).toBe("local user");
});

test("does not acknowledge a peer marker rejected by native", async ({ page }) => {
	await page.route("**/vdo-peer-sync?**", async (route) => {
		await route.fulfill({ status: 400, contentType: "application/json", body: JSON.stringify({ ok: false }) });
	});
	await page.goto(helperUrl({ label: "local-user", vdoSyncUserKey: "local-user" }));
	const frame = await vdoFrame(page);
	await frame.evaluate(() => { window.received = []; });
	await frame.evaluate(() => {
		parent.postMessage({
			dataReceived: {
				ninjamPlusSync: {
					version: 1,
					signalType: "intervalSyncTag",
					payload: {
						type: "intervalSyncTag",
						userId: "remote-user",
						intervalIndex: 1,
						bpi: 16,
						beatIndex: 0,
						eventId: "intervalTag:remote-user:rejected",
					},
				},
			},
			UUID: "remote-peer-uuid",
		}, "*");
	});
	await page.waitForTimeout(300);
	expect(await frame.evaluate(() => window.received.some((message) => message?.sendData?.ninjamPlusSync?.signalType === "intervalSyncAck"))).toBe(false);
});

test("shows a green peer light after bidirectional P2P sync produces a buffer", async ({ page }) => {
	await page.addInitScript(() => {
		window.createdWebSockets = [];
		window.WebSocket = class MockWebSocket {
			constructor(url) {
				this.url = url;
				window.createdWebSockets.push(this);
				setTimeout(() => this.onopen && this.onopen(), 0);
			}
			close() {}
		};
	});
	await page.goto(helperUrl({
		label: "local-user",
		vdoSyncUserKey: "local-user",
		intervalSource: "ws://sync.test/intervals",
	}));
	const frame = await vdoFrame(page);
	const appFrame = frame.parentFrame();
	await expect.poll(() => appFrame.evaluate(() => window.createdWebSockets.length)).toBeGreaterThan(0);
	await frame.evaluate(() => parent.postMessage({ streamIDs: { remote_stream: "remote-user" } }, "*"));
	await appFrame.evaluate(() => {
		window.createdWebSockets[0].onmessage({
			data: JSON.stringify({
				type: "videoTimecode",
				userId: "remote-user",
				interval: 3,
				timecode: 0,
				intervalMeasurementSeen: true,
				bufferCalculated: true,
				receiverBufferMs: 800,
				receiverBufferFinal: true,
				serverRouteLatencyReady: true,
				vdoPeerSyncReceived: true,
				vdoPeerSyncAcked: true,
				syncRoute: "VDO",
			}),
		});
	});
	const light = appFrame.locator(".peer-light.ok").first();
	await expect(light).toBeVisible();
	await expect(light).toHaveAttribute("aria-label", /p2p ok/);
	await expect(appFrame.locator(".peer-route").filter({ hasText: "VDO" }).first()).toBeVisible();
});

test("shows the successful sync route beside the outer page peer light", async ({ page }) => {
	await page.route("**/intervals", async (route) => {
		await route.fulfill({
			status: 200,
			contentType: "application/json",
			body: JSON.stringify([{
				type: "videoTimecode",
				userId: "route-user",
				userKey: "route-user",
				interval: 2,
				timecode: 0,
				intervalMeasurementSeen: true,
				bufferCalculated: true,
				receiverBufferMs: 700,
				receiverBufferFinal: true,
				syncRoute: "SIDE",
			}]),
		});
	});
	await page.goto(helperUrl());
	const routeBadge = page.locator("#peer-sync-route-route-user");
	await expect(routeBadge).toHaveText("SIDE");
	await expect(routeBadge).toBeVisible();
});

test("shows which sync message direction is missing beside a red light", async ({ page }) => {
	await page.route("**/intervals", async (route) => {
		await route.fulfill({
			status: 200,
			contentType: "application/json",
			body: JSON.stringify([
				{ type: "videoTimecode", userId: "missing-receive", interval: 1, syncMessageReceived: false, syncMessageAcked: true },
				{ type: "videoTimecode", userId: "missing-send", interval: 1, syncMessageReceived: true, syncMessageAcked: false },
				{ type: "videoTimecode", userId: "missing-both", interval: 1, syncMessageReceived: false, syncMessageAcked: false },
			]),
		});
	});
	await page.goto(helperUrl());
	await expect(page.locator("#peer-sync-route-missing-receive")).toHaveText("NO RCV MSG");
	await expect(page.locator("#peer-sync-route-missing-send")).toHaveText("NO SENT MSG");
	await expect(page.locator("#peer-sync-route-missing-both")).toHaveText("NO RCV / NO SENT");
});
