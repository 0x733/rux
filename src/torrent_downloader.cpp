#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

extern "C" int torrent_downloader_download(const char *torrent_or_magnet, const char *output_dir, bool quiet) {
    try {
        lt::session ses;
        lt::add_torrent_params p;
        std::string input(torrent_or_magnet);
        if (input.rfind("magnet:", 0) == 0) {
            p = lt::parse_magnet_uri(input);
        } else {
            p.ti = std::make_shared<lt::torrent_info>(input);
        }
        p.save_path = output_dir && *output_dir ? output_dir : ".";
        lt::torrent_handle h = ses.add_torrent(p);
        if (!quiet) {
            std::cout << "\033[1;36mTorrent added:\033[0m \033[34m" << torrent_or_magnet << "\033[0m" << std::endl;
            std::cout << "\033[1;36mSaving to:\033[0m \033[32m" << p.save_path << "\033[0m" << std::endl;
        }
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            lt::torrent_status s = h.status();
            if (s.state == lt::torrent_status::checking_files) {
                if (!quiet) {
                    std::cerr << "\r\033[KChecking files..." << std::flush;
                }
                continue;
            }
            if (s.state == lt::torrent_status::downloading_metadata) {
                if (!quiet) {
                    std::cerr << "\r\033[KDownloading metadata..." << std::flush;
                }
                continue;
            }
            double progress = s.progress * 100.0;
            double dl_speed = s.download_rate;
            double ul_speed = s.upload_rate;
            char speed_str[32];
            if (dl_speed < 1024) {
                snprintf(speed_str, sizeof(speed_str), "%.1f B/s", dl_speed);
            } else if (dl_speed < 1024 * 1024) {
                snprintf(speed_str, sizeof(speed_str), "%.1f KB/s", dl_speed / 1024.0);
            } else {
                snprintf(speed_str, sizeof(speed_str), "%.1f MB/s", dl_speed / (1024.0 * 1024.0));
            }
            char ul_speed_str[32];
            if (ul_speed < 1024) {
                snprintf(ul_speed_str, sizeof(ul_speed_str), "%.1f B/s", ul_speed);
            } else if (ul_speed < 1024 * 1024) {
                snprintf(ul_speed_str, sizeof(ul_speed_str), "%.1f KB/s", ul_speed / 1024.0);
            } else {
                snprintf(ul_speed_str, sizeof(ul_speed_str), "%.1f MB/s", ul_speed / (1024.0 * 1024.0));
            }
            if (!quiet) {
                int bar_width = 30;
                int pos = (int)((progress / 100.0) * bar_width);
                std::cerr << "\r\033[K[";
                for (int i = 0; i < bar_width; ++i) {
                    if (i < pos) std::cerr << "█";
                    else if (i == pos) std::cerr << "▉";
                    else std::cerr << " ";
                }
                char pct_str[32];
                snprintf(pct_str, sizeof(pct_str), "%.1f%%", progress);
                std::cerr << "] " << pct_str << " | DL: \033[1;32m" << speed_str << "\033[0m | UL: \033[1;33m" << ul_speed_str << "\033[0m" << std::flush;
            }
            if (s.is_seeding || s.state == lt::torrent_status::finished) {
                break;
            }
        }
        if (!quiet) {
            std::cerr << std::endl;
            std::cout << "\033[1;32mTorrent download complete\033[0m" << std::endl;
        }
        return 0;
    } catch (std::exception const& e) {
        std::cerr << "\033[31mError: " << e.what() << "\033[0m" << std::endl;
        return 1;
    }
}
