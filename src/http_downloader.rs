// Copyright (c) 2026 rux contributors
// Licensed under the MIT License.

use anyhow::{Context, Result};
use colored::*;
use futures_util::StreamExt;
use indicatif::{MultiProgress, ProgressBar, ProgressStyle};
use reqwest::Client;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::fs::OpenOptions;
use tokio::io::{AsyncSeekExt, AsyncWriteExt};

pub async fn download(url: &str, connections: usize, output: Option<PathBuf>, quiet: bool) -> Result<()> {
    let client = Client::new();

    // 1. Get file info
    let res = client.head(url).send().await.context("Failed to send HEAD request")?;
    let content_length = res
        .headers()
        .get(reqwest::header::CONTENT_LENGTH)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<u64>().ok());

    let accept_ranges = res
        .headers()
        .get(reqwest::header::ACCEPT_RANGES)
        .and_then(|v| v.to_str().ok())
        == Some("bytes");

    let mut file_name = output.unwrap_or_else(|| {
        url.split('/')
            .last()
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("downloaded_file"))
    });

    if file_name.is_dir() {
        let name = url.split('/')
            .last()
            .filter(|s| !s.is_empty())
            .unwrap_or("downloaded_file");
        file_name.push(name);
    }

    let total_size = match content_length {
        Some(size) => size,
        None => {
            if !quiet { println!("{}", "Content-Length unknown. Falling back to single connection.".yellow()); }
            return download_single(client, url, file_name, quiet).await;
        }
    };

    if !accept_ranges || connections <= 1 {
        if connections > 1 && !accept_ranges && !quiet {
            println!("{}", "Server does not support ranges. Falling back to single connection.".yellow());
        }
        return download_single(client, url, file_name, quiet).await;
    }

    // 2. Multi-connection download
    if !quiet {
        println!("{} {}", "Downloading:".bold().cyan(), file_name.display().to_string().green());
    }

    let m = MultiProgress::new();
    let style = ProgressStyle::default_bar()
        .template("{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {bytes}/{total_bytes} ({eta})")
        .unwrap()
        .progress_chars("#>-");

    let overall_pb = m.add(ProgressBar::new(total_size));
    overall_pb.set_style(ProgressStyle::default_bar()
        .template("{spinner:.green} [{elapsed_precise}] [{bar:40.magenta/red}] {msg} {bytes}/{total_bytes} ({eta})")
        .unwrap()
        .progress_chars("█▉▊▋▌▍▎▏  "));
    overall_pb.set_message("Overall:");

    let file = Arc::new(tokio::sync::Mutex::new(
        OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&file_name)
            .await
            .context("Failed to create file")?,
    ));

    // Pre-allocate file size
    file.lock().await.set_len(total_size).await?;

    let chunk_size = total_size / connections as u64;
    let mut tasks = Vec::new();

    for i in 0..connections {
        let start = i as u64 * chunk_size;
        let end = if i == connections - 1 {
            total_size - 1
        } else {
            (i as u64 + 1) * chunk_size - 1
        };

        let client = client.clone();
        let url = url.to_string();
        let file = Arc::clone(&file);
        let pb = m.add(ProgressBar::new(end - start + 1));
        pb.set_style(style.clone());
        let overall_pb_clone = overall_pb.clone();

        tasks.push(tokio::spawn(async move {
            let range = format!("bytes={}-{}", start, end);
            let res = client
                .get(url)
                .header(reqwest::header::RANGE, range)
                .send()
                .await?;

            let mut stream = res.bytes_stream();
            let mut current_pos = start;

            while let Some(item) = stream.next().await {
                let chunk = item?;
                let chunk_len = chunk.len() as u64;

                let mut f = file.lock().await;
                f.seek(std::io::SeekFrom::Start(current_pos)).await?;
                f.write_all(&chunk).await?;
                
                current_pos += chunk_len;
                pb.inc(chunk_len);
                overall_pb_clone.inc(chunk_len);
            }
            pb.finish_and_clear();
            Ok::<(), anyhow::Error>(())
        }));
    }

    for task in tasks {
        task.await.context("Task panicked")??;
    }

    overall_pb.finish_with_message("Complete!".bold().green().to_string());
    Ok(())
}

async fn download_single(client: Client, url: &str, file_name: PathBuf, quiet: bool) -> Result<()> {
    let res = client.get(url).send().await.context("Failed to send GET request")?;
    let total_size = res.content_length();

    let pb = if quiet {
        ProgressBar::hidden()
    } else {
        let pb = total_size.map(ProgressBar::new).unwrap_or_else(ProgressBar::new_spinner);
        pb.set_style(ProgressStyle::default_bar()
            .template("{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {bytes}/{total_bytes} ({eta})")
            .unwrap()
            .progress_chars("#>-"));
        pb
    };

    let mut file = tokio::fs::File::create(&file_name).await.context("Failed to create file")?;
    let mut stream = res.bytes_stream();

    while let Some(item) = stream.next().await {
        let chunk = item?;
        file.write_all(&chunk).await?;
        pb.inc(chunk.len() as u64);
    }

    pb.finish_with_message("Download complete".bold().green().to_string());
    Ok(())
}
