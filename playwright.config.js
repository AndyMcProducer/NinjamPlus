const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
	testDir: "./tests",
	// Keep browser cleanup from deleting native audio captures and validation logs.
	outputDir: "./test-results/playwright",
	timeout: 30000,
	expect: { timeout: 5000 },
	workers: 1,
	reporter: "list",
	webServer: {
		command: "node tests/helper-server.js",
		url: "http://127.0.0.1:8188/intervals",
		reuseExistingServer: false,
		timeout: 10000,
	},
	use: {
		...devices["Desktop Chrome"],
		baseURL: "http://127.0.0.1:8188",
		headless: true,
	},
});
