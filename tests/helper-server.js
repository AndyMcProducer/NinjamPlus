const fs = require("fs");
const http = require("http");
const path = require("path");

const root = path.resolve(__dirname, "..");
const clientRoot = path.join(root, "advanced-vdo-client");

function send(response, status, contentType, body) {
	response.writeHead(status, {
		"Content-Type": contentType,
		"Cache-Control": "no-store",
	});
	response.end(body);
}

http
	.createServer((request, response) => {
		const url = new URL(request.url, "http://127.0.0.1:8188");
		if (url.pathname === "/" || url.pathname === "/buffer-room" || url.pathname === "/index.html") {
			return send(response, 200, "text/html; charset=utf-8", fs.readFileSync(path.join(clientRoot, "index.html")));
		}
		if (url.pathname === "/app") {
			return send(response, 200, "text/html; charset=utf-8", fs.readFileSync(path.join(clientRoot, "app.html")));
		}
		if (url.pathname === "/intervals") {
			return send(response, 200, "application/json; charset=utf-8", "[]");
		}
		if (url.pathname === "/vdo-stub/") {
			return send(
				response,
				200,
				"text/html; charset=utf-8",
				"<!doctype html><script>window.received=[];addEventListener('message',function(e){received.push(e.data);});parent.postMessage({streamIDs:{}},'*');</script>"
			);
		}
		const fileName = url.pathname.replace(/^\//, "");
		const filePath = path.join(clientRoot, fileName);
		if (filePath.startsWith(clientRoot) && fs.existsSync(filePath)) {
			return send(response, 200, "application/octet-stream", fs.readFileSync(filePath));
		}
		send(response, 404, "text/plain; charset=utf-8", "Not found");
	})
	.listen(8188, "127.0.0.1");
