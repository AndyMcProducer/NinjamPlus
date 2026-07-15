const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const outputPath = path.join(root, "Source", "EmbeddedVdoHtml.h");
const assets = [
	["vdoIndexHtml", "advanced-vdo-client/index.html"],
	["vdoAppHtml", "advanced-vdo-client/app.html"],
	["vdoIconPng", "advanced-vdo-client/icon.png"],
	["vdoPoweredByPng", "advanced-vdo-client/PoweredByVDONinja.png"],
	["vdoCloudMaskPng", "advanced-vdo-client/masks/cloud.png"],
];

function renderArray(name, fileName) {
	const bytes = fs.readFileSync(path.join(root, fileName));
	const lines = [];
	for (let offset = 0; offset < bytes.length; offset += 16) {
		const values = Array.from(bytes.subarray(offset, offset + 16), (value) =>
			"0x" + value.toString(16).padStart(2, "0")
		);
		lines.push("    " + values.join(", ") + (offset + 16 < bytes.length ? "," : ""));
	}
	return [
		"inline constexpr unsigned char " + name + "[] =",
		"{",
		lines.join("\n"),
		"};",
		"inline constexpr std::size_t " + name + "Size = sizeof(" + name + ");",
	].join("\n");
}

const generated = [
	"// Generated from advanced-vdo-client HTML and image assets.",
	"// Run `npm run embed:vdo` after editing those source files.",
	"#pragma once",
	"",
	"#include <cstddef>",
	"",
	"namespace ninjamplus::embedded",
	"{",
	assets.map(([name, fileName]) => renderArray(name, fileName)).join("\n"),
	"}",
	"",
].join("\n");

if (process.argv.includes("--check")) {
	const current = fs.existsSync(outputPath) ? fs.readFileSync(outputPath, "utf8").replace(/\r\n/g, "\n") : "";
	if (current !== generated) {
		console.error("Source/EmbeddedVdoHtml.h is stale; run `npm run embed:vdo`.");
		process.exit(1);
	}
	console.log("Embedded VDO helper assets are current.");
} else {
	fs.writeFileSync(outputPath, generated);
	console.log("Updated " + path.relative(root, outputPath));
}
