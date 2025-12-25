// See: https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#javascript-limits-in-library-files
addToLibrary({
	$snet_transport_init__postset: 'snet_transport_init();',
	$snet_transport_init: () => {
		const KeepAliveMs = 2000;
		const KeepAliveBuf = new ArrayBuffer(0);

		const handles = new Map();
		let nextHandle = 0;

		_snet_transport_impl_connect = (config) => {
			const handle = nextHandle++;
			const transport = {
				state: 1,
				messages: [],
				sendDatagram: () => {},
				sendStream: () => {},
				close: () => {},
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

		_snet_transport_impl_recv = (handle, message_ptr, size_ptr) => {
			const transport = handles.get(handle);
			if (transport && transport.state === 2) {
				const message = transport.messages.shift();
				if (message) {
					const dest = _malloc(message.length);
					HEAPU8.set(message, dest);
					// TODO: how to detect WASM64?
					setValue(message_ptr, dest, 'i32');
					setValue(size_ptr, message.length, 'i32');
					return true;
				} else {
					setValue(message_ptr, 0, 'i32');
					setValue(size_ptr, 0, 'i32');
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
					transport.sendDatagram(HEAPU8.subarray(message, message + size)).catch(() => {});
				} else {
					const frame = new Uint8Array(size + 2);
					const view = new DataView(frame.buffer);
					view.setUint16(0, size, true);
					frame.set(HEAPU8.subarray(message, message + size), 2);

					transport.sendStream(frame).catch(() => {});
				}
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
			let reliableStream = null;

			for (const url of config.urls) {
				wt = new WebTransport(url, {
					congestionControl: 'low-latency',
					requireUnreliable: true,
					serverCertificateHashes,
				});

				try {
					await wt.ready;
					const { done, value } = await wt.incomingBidirectionalStreams.getReader().read();
					reliableStream = value;
				} catch (err) {
					wt.close();
					wt = null;
					continue;
				}
				break;
			}

			if (wt !== null) {
				transport.state = 2;

				const datagramWriter = wt.datagrams.writable.getWriter();
				transport.sendDatagram = datagramWriter.write.bind(datagramWriter);

				const streamWriter = reliableStream.writable.getWriter();
				transport.sendStream = streamWriter.write.bind(streamWriter);

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

				const datagramReader = (async () => {
					const reader = wt.datagrams.readable.getReader();
					while (!abortSignal.abort) {
						try {
							const { value, done } = await Promise.race([reader.read(), abortPromise]);
							if (done) { break; }

							transport.messages.push(value);
						} catch (err) {
							break;
						}
					}
				})().catch(console.err);

				const streamReader = (async () => {
					const reader = reliableStream.readable.getReader();
					let buffer = new Uint8Array(0);

					while (!abortSignal.abort) {
						const { value, done } = await Promise.race([reader.read(), abortPromise]);
						if (done) { break; }

						// Concatenate leftover bytes with new chunk
						const chunk = value ? value : new Uint8Array(0);
						const combined = new Uint8Array(buffer.length + chunk.length);
						combined.set(buffer, 0);
						combined.set(chunk, buffer.length);
						buffer = combined;

						let offset = 0;
						while (buffer.length - offset >= 2) {
							const view = new DataView(buffer.buffer, offset, 2);
							const msgLength = view.getUint16(0, true);
							if (buffer.length - offset - 2 >= msgLength) {
								const message = buffer.slice(offset + 2, offset + 2 + msgLength);
								transport.messages.push(message);
								offset += 2 + msgLength;
							} else {
								break;
							}
						}

						buffer = buffer.slice(offset);
					}
				})().catch(console.err);

				await Promise.race([
					datagramReader,
					streamReader,
				]);
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
	snet_transport_impl_recv: () => {},
	snet_transport_impl_recv__deps: ['$snet_transport_init'],
	snet_transport_impl_send: () => {},
	snet_transport_impl_send__deps: ['$snet_transport_init'],
});
