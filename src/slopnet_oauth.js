// See: https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#javascript-limits-in-library-files
addToLibrary({
	$snet_oauth_init__postset: 'snet_oauth_init();',
	$snet_oauth_init: () => {
		let data = null;
		let state = 0;

		const onMessage = (message) => {
			state = message.data.success == "1" ? 1 : 2;
			data = message.data.data;
			window.removeEventListener("message", onMessage);
		}

		_snet_oauth_begin_js = (url) => {
			data = null;
			state = 0;
			window.addEventListener("message", onMessage);
			window.open(UTF8ToString(url) + "?origin=" + window.origin);
		};

		_snet_oauth_update = () => {
			return state;
		};

		_snet_oauth_data_size = () => {
			return data ? data.length * 4 + 1 : 0;
		}

		_snet_oauth_copy_data = (ptr, max_size) => {
			if (data) {
				stringToUTF8(data, ptr, max_size);
			}
		}
	},
	snet_oauth_begin_js: () => {},
	snet_oauth_begin_js__deps: ['$snet_oauth_init'],
	snet_oauth_update: () => {},
	snet_oauth_update__deps: ['$snet_oauth_init'],
	snet_oauth_data_size: () => {},
	snet_oauth_data_size__deps: ['$snet_oauth_init'],
	snet_oauth_copy_data: () => {},
	snet_oauth_copy_data__deps: ['$snet_oauth_init'],
});
