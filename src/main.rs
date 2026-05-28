// Copyright (c) 2026 rux contributors
// Licensed under the MIT License.

use anyhow::Result;
use clap::Parser;
use colored::*;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(author, version, about = "A high-performance download manager for HTTP and Torrents")]
struct Args {
    #[arg(required = true)]
    inputs: Vec<String>,

    #[arg(short = 'n', long, default_value_t = 4)]
    connections: usize,

    #[arg(short = 'o', long)]
    output: Option<PathBuf>,

    #[arg(short = 'q', long)]
    quiet: bool,

    #[arg(short = 's', long)]
    max_speed: Option<String>,

    #[arg(short = 'U', long)]
    user_agent: Option<String>,

    #[arg(short = 'c', long)]
    resume: bool,

    #[arg(short = 'H', long = "header")]
    headers: Vec<String>,

    #[arg(long)]
    proxy: Option<String>,

    #[arg(long)]
    checksum: Option<String>,
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
        println!("{:<15} {}", "Input:".bold().yellow(), args.inputs[0].blue());
        println!("{:<15} {}", "Connections:".bold().yellow(), args.connections.to_string().magenta());
        if let Some(ref o) = args.output {
            println!("{:<15} {}", "Output:".bold().yellow(), o.display().to_string().green());
        }
        println!("{}", "-".repeat(60).dimmed());
    }

    if args.inputs.is_empty() {
        anyhow::bail!("No inputs provided.");
    }
    let primary_input = &args.inputs[0];

    if primary_input.starts_with("magnet:") || primary_input.ends_with(".torrent") {
        torrent_downloader::download(primary_input, args.output, args.quiet).await?;
    } else if primary_input.starts_with("http://") || primary_input.starts_with("https://") {
        let parsed_speed = args.max_speed.as_deref().and_then(parse_speed);
        let urls_joined = args.inputs.join("\n");
        let headers_joined = args.headers.join("\n");
        let final_path = http_downloader::download(
            &urls_joined,
            args.connections,
            args.output,
            args.quiet,
            parsed_speed,
            args.user_agent,
            args.resume,
            if headers_joined.is_empty() { None } else { Some(headers_joined) },
            args.proxy,
        ).await?;

        if let Some(ref expected_checksum) = args.checksum {
            verify_checksum(&final_path, expected_checksum)?;
        }
    } else {
        anyhow::bail!("Unsupported input format. Please provide an HTTP/HTTPS URL, a Magnet link, or a path to a .torrent file.");
    }

    Ok(())
}

fn verify_checksum(path: &std::path::Path, checksum_arg: &str) -> Result<()> {
    let mut parts = checksum_arg.splitn(2, ':');
    let (algo, expected) = match (parts.next(), parts.next()) {
        (Some(a), Some(e)) => (a.to_lowercase(), e.to_lowercase()),
        (Some(e), None) => ("sha256".to_string(), e.to_lowercase()),
        _ => anyhow::bail!("Invalid checksum format. Use <algo>:<expected_hash>"),
    };
    let mut file = std::fs::File::open(path)?;
    let mut buffer = vec![0; 65536];
    let actual = match algo.as_str() {
        "sha256" => {
            use sha2::{Digest, Sha256};
            let mut hasher = Sha256::new();
            loop {
                let bytes_read = std::io::Read::read(&mut file, &mut buffer)?;
                if bytes_read == 0 {
                    break;
                }
                hasher.update(&buffer[..bytes_read]);
            }
            format!("{:x}", hasher.finalize())
        }
        "sha512" => {
            use sha2::{Digest, Sha512};
            let mut hasher = Sha512::new();
            loop {
                let bytes_read = std::io::Read::read(&mut file, &mut buffer)?;
                if bytes_read == 0 {
                    break;
                }
                hasher.update(&buffer[..bytes_read]);
            }
            format!("{:x}", hasher.finalize())
        }
        "md5" => {
            let mut context = md5::Context::new();
            loop {
                let bytes_read = std::io::Read::read(&mut file, &mut buffer)?;
                if bytes_read == 0 {
                    break;
                }
                context.consume(&buffer[..bytes_read]);
            }
            format!("{:x}", context.compute())
        }
        _ => anyhow::bail!("Unsupported checksum algorithm: {}. Use sha256, sha512, or md5", algo),
    };
    if actual == expected {
        println!("{}", "Checksum verification successful!".bold().bright_green());
        Ok(())
    } else {
        anyhow::bail!(
            "Checksum mismatch!\nExpected: {}\nActual:   {}",
            expected.red(),
            actual.red()
        );
    }
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

