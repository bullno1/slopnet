// See: https://emscripten.org/docs/porting/connecting_cpp_and_javascript/Interacting-with-code.html#javascript-limits-in-library-files
addToLibrary({
	$snet_oauth_init__postset: 'snet_oauth_init();',
	$snet_oauth_init: () => {
		class Lib {
			constructor() {
				this.data = null;
				this.state = 0;
				this._onMessage = this.onMessage.bind(this);
			}

			begin(url) {
				this.data = null;
				this.state = 0;
				window.addEventListener("message", this._onMessage);
				window.open(UTF8ToString(url) + "?origin=" + window.origin);
			}

			update() {
				return this.state;
			}

			data_size() {
				return this.data ? this.data.length * 4 + 1 : 0;
			}

			copy_data(ptr, max_size) {
				if (this.data) {
					stringToUTF8(this.data, ptr, max_size);
				}
			}

			onMessage(message) {
				this.state = message.data.success == "1" ? 1 : 2;
				this.data = message.data.data;
				window.removeEventListener("message", this._onMessage);
			}
		}

		const lib = new Lib();
		_snet_oauth_begin_js = lib.begin.bind(lib);
		_snet_oauth_update = lib.update.bind(lib);
		_snet_oauth_data_size = lib.data_size.bind(lib);
		_snet_oauth_copy_data = lib.copy_data.bind(lib);
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
