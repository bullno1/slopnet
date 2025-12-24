// See: https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#javascript-limits-in-library-files
addToLibrary({
	$snet_transport_init__postset: 'snet_transport_init();',
	$snet_transport_init: () => {
		const handles = new Map();
		let nextHandle = 0;

		_snet_transport_impl_connect = (config) => {
			const handle = nextHandle++;
			const transport = { state: 1, messages: [] };
			handles.set(handle, transport);

			start(transport, config).catch((err) => {
				console.error(err);
				transport.state = 0;
			});

			return handle;
		};

		_snet_transport_impl_disconnect = (handle) => {
			const transport = handles.get(handle);
			if (transport) {
				handles.delete(handle);
				if (transport.close) {
					transport.close();
				}

				console.log("Closed");
			}
		};

		_snet_transport_impl_state = (handle) => {
			const transport = handles.get(handle);
			return transport ? transport.state : 0;
		};

		_snet_transport_impl_recv = (handle, message_ptr, size_ptr) => {
			const transport = handles.get(handle);
			if (transport) {
				const message = transport.messages.shift();
				if (message) {
					const dest = _malloc(message.length);
					HEAPU8.set(message, dest);
					setValue(message_ptr, dest, 'number');
					setValue(size_ptr, message.length, 'number');
					return true;
				} else {
					return false;
				}
			} else {
				return false;
			}
		};

		_snet_transport_impl_send = (handle, message, size, reliable) => {
			const transport = handles.get(handle);
			if (transport && transport.state === 2) {
				if (!reliable) {
					transport.send(HEAPU8.subarray(message, message + size));
				} else {
					// TODO: implement reliable
				}
			}
		}

		const start = async (transport, config_ptr) => {
			const configStr = UTF8ToString(config_ptr);
			console.log(configStr);
			const config = JSON.parse(UTF8ToString(config_ptr));
			const serverCertificateHashes = config.serverCertificateHashes.map((b64) => ({
				algorithm: 'sha-256',
				value: Uint8Array.fromBase64(b64),
			}));

			let wt = null;

			for (const url of config.urls) {
				wt = new WebTransport(url, {
					congestionControl: 'low-latency',
					requireUnreliable: true,
					serverCertificateHashes,
				});

				console.log("Trying", url);
				try {
					await wt.ready;
				} catch (err) {
					wt = null;
					continue;
				}
				break;
			}

			if (wt !== null) {
				transport.state = 2;

				wt.closed.then(() => transport.state = 0);
				const writer = wt.datagrams.writable.getWriter();
				transport.send = writer.write.bind(writer);
				transport.close = () => {
					if (transport.state === 2) {
						wt.close();
					} else if (transport.state === 1) {
						transport.ready.then(() => wt.close());
					}
				};

				const reader = wt.datagrams.readable.getReader();
				while (true) {
					console.log("Reading");
					const result = await reader.read();
					if (result.done) { break; }

					transport.messages.push(result.value);
				}
			}

			console.log("End");
			transport.state = 0;
		};
	},
	snet_transport_impl_connect: () => {},
	snet_transport_impl_connect__deps: ['$snet_transport_init'],
	snet_transport_impl_disconnect: () => {},
	snet_transport_impl_disconnect__deps: ['$snet_transport_init'],
	snet_transport_impl_state: () => {},
	snet_transport_impl_state__deps: ['$snet_transport_init'],
	snet_transport_impl_recv: () => {},
	snet_transport_impl_recv__deps: ['$snet_transport_init'],
	snet_transport_impl_send: () => {},
	snet_transport_impl_send__deps: ['$snet_transport_init'],
});
