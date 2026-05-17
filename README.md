# rux

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Year](https://img.shields.io/badge/year-2026-brightgreen.svg)

**rux** is a high-performance download manager written in Rust, designed for 2026 standards. It supports both multi-connection HTTP/HTTPS downloads and modern Torrent/Magnet protocols.

## Features

- **Multi-Connection HTTP:** Downloads files in segments to maximize speed.
- **Full Torrent/Magnet Support:** Powered by the `librqbit` engine for fast and reliable peer-to-peer downloads.
- **Modern CLI:** Aesthetic, colorful, and detailed progress bars suited for 2026 terminal environments.
- **Unix Philosophy:** Simple, fast, and compatible with other tools.

## Installation

Ensure you have Rust and Cargo installed, then:

```bash
git clone git@github.com:0x733/rux.git
cd rux
cargo build --release
```

## Usage

### HTTP Download
```bash
./target/release/rux https://example.com/very-large-file.iso -n 8
```

### Torrent/Magnet Download
```bash
./target/release/rux "magnet:?xt=urn:btih:..."
```

## License

This project is licensed under the [MIT License](./LICENSE). © 2026
