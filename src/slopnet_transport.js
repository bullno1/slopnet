// See: https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#javascript-limits-in-library-files
addToLibrary({
	$snet_transport_init__postset: 'snet_transport_init();',
	$snet_transport_init: () => {
		const KeepAliveMs = 1000;
		const KeepAliveBuf = new ArrayBuffer(0);

		const handles = new Map();
		let nextHandle = 0;

		_snet_transport_impl_connect = (config, recvBuf, context) => {
			const handle = nextHandle++;
			const transport = {
				state: 1,
				sendDatagram: () => {},
				close: () => {},
				maxDatagramSize: 0,
				recvBuf: recvBuf,
				context: context,
				lastSend: performance.now(),
			};
			handles.set(handle, transport);

			start(transport, config).catch((err) => {
				console.error(err);
			}).finally(() => {
				transport.close();
				transport.state = 0;
			});

			return handle;
		};

		_snet_transport_impl_disconnect = (handle) => {
			const transport = handles.get(handle);
			if (transport) {
				handles.delete(handle);
				transport.close();
			}
		};

		_snet_transport_impl_state = (handle) => {
			const transport = handles.get(handle);
			return transport ? transport.state : 0;
		};

		_snet_transport_impl_max_datagram_size = (handle) => {
			const transport = handles.get(handle);
			return transport ? transport.maxDatagramSize : 0;
		}

		const bufferPool = [];

		const acquireSendBuffer = (data) => {
			let buffer = bufferPool.pop();
			if (buffer === undefined) {
				buffer = new ArrayBuffer(2048);
			}

			const view = new Uint8Array(buffer, 0, data.length);
			view.set(data);
			return view;
		};

		const acquireReadBuffer = () => {
			let buffer = bufferPool.pop();
			if (buffer === undefined) {
				buffer = new ArrayBuffer(2048);
			}

			return new Uint8Array(buffer);
		};

		const releaseBuffer = (array) => {
			bufferPool.push(array.buffer);
		};

		_snet_transport_impl_send = (handle, message, size) => {
			const transport = handles.get(handle);
			if (transport && transport.state === 2) {
				// Sending is async so we need a buffer pool to copy messages
				const copy = acquireSendBuffer(HEAPU8.subarray(message, message + size));
				transport.sendDatagram(copy).then(() => {
					releaseBuffer(copy);
				}).catch((err) => {
					console.error(err);
					transport.close();
				});
				transport.lastSend = performance.now();
			}
		}

		const start = async (transport, config_ptr) => {
			const configStr = UTF8ToString(config_ptr);
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

				try {
					await wt.ready;
				} catch (err) {
					wt.close();
					wt = null;
					continue;
				}
				break;
			}

			if (wt !== null) {
				transport.state = 2;
				transport.maxDatagramSize = wt.datagrams.maxDatagramSize;

				const datagramWriter = wt.datagrams.writable.getWriter();
				transport.sendDatagram = datagramWriter.write.bind(datagramWriter);

				const keepAliveTimer = setInterval(() => {
					const now = performance.now();
					if (now - transport.lastSend >= KeepAliveMs) {
						transport.sendDatagram(KeepAliveBuf).catch(() => {});
						transport.lastSend = now;
					}
				}, KeepAliveMs);

				const abortController = new AbortController();
				const abortSignal = abortController.signal;
				const abortPromise = new Promise((resolve, reject) => {
					abortSignal.addEventListener("abort", reject);
				});

				transport.close = () => {
					clearInterval(keepAliveTimer);
					abortController.abort();
					if (transport.state === 2) {
						try { wt.close(); } catch (err) { }
					} else if (transport.state === 1) {
						wt.ready.then(() => wt.close()).catch(() => {});
					}
					transport.state = 0;
				};

				wt.closed.finally(() => { transport.state = 0; transport.close(); });

				const reader = wt.datagrams.readable.getReader({ mode: "byob" });
				while (!abortSignal.abort) {
					try {
						const { value, done } = await Promise.race([
							reader.read(acquireReadBuffer()),
							abortPromise
						]);
						if (abortSignal.abort) { break; }
						if (done) { break; }

						HEAPU8.set(value, transport.recvBuf);
						_snet_transport_process_incoming(transport.context, value.length);
						releaseBuffer(value);
					} catch (err) {
						console.error(err);
						break;
					}
				}
				await reader.cancel().catch(() => {});
			}

			transport.state = 0;
		};
	},
	snet_transport_impl_connect: () => {},
	snet_transport_impl_connect__deps: ['$snet_transport_init'],
	snet_transport_impl_disconnect: () => {},
	snet_transport_impl_disconnect__deps: ['$snet_transport_init'],
	snet_transport_impl_state: () => {},
	snet_transport_impl_state__deps: ['$snet_transport_init'],
	snet_transport_impl_max_datagram_size: () => {},
	snet_transport_impl_max_datagram_size__deps: ['$snet_transport_init'],
	snet_transport_impl_recv: () => {},
	snet_transport_impl_recv__deps: ['$snet_transport_init'],
	snet_transport_impl_send: () => {},
	snet_transport_impl_send__deps: ['$snet_transport_init'],
});
