// Copyright (c) 2026 rux contributors
// Licensed under the MIT License.

use anyhow::Result;
use clap::Parser;
use colored::*;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(author, version, about = "A high-performance download manager for HTTP and Torrents")]
struct Args {
    /// URL, Magnet link, or path to a .torrent file
    #[arg(required = true)]
    input: String,

    /// Number of connections for HTTP downloads
    #[arg(short = 'n', long, default_value_t = 4)]
    connections: usize,

    /// Output file or directory
    #[arg(short = 'o', long)]
    output: Option<PathBuf>,

    /// Quiet mode (no progress bar)
    #[arg(short = 'q', long)]
    quiet: bool,

    #[arg(short = 's', long)]
    max_speed: Option<String>,

    #[arg(short = 'U', long)]
    user_agent: Option<String>,

    #[arg(short = 'c', long)]
    resume: bool,
}

mod http_downloader;
mod torrent_downloader;

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    if !args.quiet {
        println!("{}", "=".repeat(60).cyan());
        println!(" {} ", "RUX - High Performance Downloader".bold().bright_green());
        println!("{}", "=".repeat(60).cyan());
        println!("{:<15} {}", "Input:".bold().yellow(), args.input.blue());
        println!("{:<15} {}", "Connections:".bold().yellow(), args.connections.to_string().magenta());
        if let Some(ref o) = args.output {
            println!("{:<15} {}", "Output:".bold().yellow(), o.display().to_string().green());
        }
        println!("{}", "-".repeat(60).dimmed());
    }

    if args.input.starts_with("magnet:") || args.input.ends_with(".torrent") {
        torrent_downloader::download(&args.input, args.output, args.quiet).await?;
    } else if args.input.starts_with("http://") || args.input.starts_with("https://") {
        let parsed_speed = args.max_speed.as_deref().and_then(parse_speed);
        http_downloader::download(
            &args.input,
            args.connections,
            args.output,
            args.quiet,
            parsed_speed,
            args.user_agent,
            args.resume,
        ).await?;
    } else {
        anyhow::bail!("Unsupported input format. Please provide an HTTP/HTTPS URL, a Magnet link, or a path to a .torrent file.");
    }

    Ok(())
}

fn parse_speed(speed_str: &str) -> Option<u64> {
    let speed_str = speed_str.trim().to_uppercase();
    if speed_str.is_empty() {
        return None;
    }
    let (num_str, unit) = if speed_str.ends_with("MB") {
        (&speed_str[..speed_str.len() - 2], 1024 * 1024)
    } else if speed_str.ends_with('M') {
        (&speed_str[..speed_str.len() - 1], 1024 * 1024)
    } else if speed_str.ends_with("KB") {
        (&speed_str[..speed_str.len() - 2], 1024)
    } else if speed_str.ends_with('K') {
        (&speed_str[..speed_str.len() - 1], 1024)
    } else {
        (speed_str.as_str(), 1)
    };
    num_str.parse::<u64>().ok().map(|n| n * unit)
}

