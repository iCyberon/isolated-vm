'use strict';
let WebSocket = require('ws');
let ivm = require('./isolated-vm');

/**
 * Note that allowing untrusted users to access the v8 inspector may result in security issues.
 * Maybe it won't though, I don't know. I haven't really looked into everything the inspector can
 * do.
 */

// Launch an infinite loop in another thread
let isolate = new ivm.Isolate({ inspector: true });
(async function() {
	let context = await isolate.createContext({ inspector: true });
	let script = await isolate.compileScript('for(;;)debugger;', { filename: 'example.js' });
	await script.run(context);
}()).catch(console.error);

// Create an inspector channel on port 10000
let channel = isolate.createInspectorSession();
let wss = new WebSocket.Server({ port: 10000 });

wss.on('connection', function(ws) {
	// Dispose inspector session on websocket disconnect
	let channel = isolate.createInspectorSession();
	function dispose() {
		try {
			channel.dispose();
		} catch (err) {}
	}
	ws.on('error', dispose);
	ws.on('close', dispose);

	// Relay messages from frontend to backend
  ws.on('message', function(message) {
		try {
			channel.dispatchProtocolMessage(message);
		} catch (err) {
			// This happens if inspector session was closed unexpectedly
			ws.close();
		}
  });

	// Relay messages from backend to frontend
	function send(message) {
		try {
			ws.send(message);
		} catch (err) {
			dispose();
		}
	}
	channel.onResponse = (callId, message) => send(message);
	channel.onNotification = send;
});
console.log('Inspector: chrome-devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=127.0.0.1:10000');
