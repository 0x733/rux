# rux

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Year](https://img.shields.io/badge/year-2026-brightgreen.svg)

**rux**, 2026 yılı standartlarına uygun, Rust ile yazılmış, yüksek performanslı bir indirme yöneticisidir. Hem HTTP/HTTPS çoklu bağlantı (multi-connection) indirmeyi hem de modern Torrent/Magnet protokollerini destekler.

## Özellikler

- **Multi-Connection HTTP:** Dosyaları parçalara bölerek maksimum hızda indirir.
- **Full Torrent/Magnet Desteği:** `librqbit` motoru ile hızlı ve güvenilir peer-to-peer indirme.
- **Modern CLI:** 2026 yılı terminal estetiğine uygun renkli ve detaylı ilerleme çubukları.
- **Unix Felsefesi:** Basit, hızlı ve diğer araçlarla uyumlu.

## Kurulum

Rust ve Cargo'nun yüklü olduğundan emin olun, ardından:

```bash
git clone https://github.com/user/rux
cd rux
cargo build --release
```

## Kullanım

### HTTP İndirme
```bash
./target/release/rux https://example.com/very-large-file.iso -n 8
```

### Torrent/Magnet İndirme
```bash
./target/release/rux "magnet:?xt=urn:btih:..."
```

## Lisans

Bu proje [MIT Lisansı](./LICENSE) altında lisanslanmıştır. © 2026
