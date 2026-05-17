// Copyright (c) 2026 rux contributors
// Licensed under the MIT License.

use anyhow::{Context, Result};
use colored::*;
use indicatif::{ProgressBar, ProgressStyle};
use librqbit::{Session, AddTorrent, AddTorrentOptions};
use std::path::PathBuf;
use std::time::Duration;

pub async fn download(input: &str, output: Option<PathBuf>, quiet: bool) -> Result<()> {
    let output_dir = output.unwrap_or_else(|| PathBuf::from("."));
    
    let session = Session::new(output_dir.clone()).await.context("Failed to create librqbit session")?;
    
    let add_torrent = AddTorrent::from_cli_argument(input).context("Failed to parse torrent input")?;
    
    let torrent = session.add_torrent(
        add_torrent,
        Some(AddTorrentOptions {
            overwrite: true,
            ..Default::default()
        })
    ).await.context("Failed to add torrent")?;

    let handle = torrent.into_handle().context("Failed to get torrent handle")?;

    if !quiet {
        println!("{} {}", "Torrent added:".bold().cyan(), input.blue());
        println!("{} {}", "Saving to:".bold().cyan(), output_dir.display().to_string().green());
    }

    let pb = if quiet {
        ProgressBar::hidden()
    } else {
        let pb = ProgressBar::new(100);
        pb.set_style(ProgressStyle::default_bar()
            .template("{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {percent}% | {msg} | {eta}")
            .unwrap()
            .progress_chars("█▉▊▋▌▍▎▏  "));
        pb
    };

    loop {
        let stats = handle.stats();
        let total_bytes = stats.total_bytes;
        let downloaded_bytes = stats.progress_bytes;
        
        if total_bytes > 0 {
            let percent = (downloaded_bytes as f64 / total_bytes as f64 * 100.0) as u64;
            pb.set_position(percent);
            
            let mut msg = format!("{}/{}", format_bytes(downloaded_bytes), format_bytes(total_bytes));
            
            if let Some(live) = stats.live {
                msg = format!("{} | DL: {} | UL: {}", 
                    msg, 
                    live.download_speed.to_string().bright_green(),
                    live.upload_speed.to_string().bright_yellow()
                );
            }
            
            pb.set_message(msg);
        }

        if stats.finished {
            break;
        }

        tokio::time::sleep(Duration::from_millis(500)).await;
    }

    pb.finish_with_message("Torrent download complete".bold().green().to_string());
    Ok(())
}

fn format_bytes(bytes: u64) -> String {
    if bytes < 1024 {
        format!("{} B", bytes)
    } else if bytes < 1024 * 1024 {
        format!("{:.2} KB", bytes as f64 / 1024.0)
    } else if bytes < 1024 * 1024 * 1024 {
        format!("{:.2} MB", bytes as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.2} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}
