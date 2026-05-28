use anyhow::{Context, Result};
use colored::*;
use indicatif::{ProgressBar, ProgressStyle};
use reqwest::Client;
use std::ffi::{c_char, c_void, CString};
use std::path::PathBuf;
use std::sync::Arc;

unsafe extern "C" {
    fn download_http_cpp(
        url: *const c_char,
        output_path: *const c_char,
        connections: i32,
        quiet: bool,
        callback: Option<unsafe extern "C" fn(*mut c_void, u64)>,
        user_data: *mut c_void,
        max_speed: i64,
        user_agent: *const c_char,
        resume: bool,
    ) -> i32;
}

unsafe extern "C" fn rust_progress_callback(user_data: *mut c_void, bytes: u64) {
    if !user_data.is_null() {
        let pb = unsafe { &*(user_data as *const ProgressBar) };
        pb.inc(bytes);
    }
}

pub async fn download(
    url: &str,
    connections: usize,
    output: Option<PathBuf>,
    quiet: bool,
    max_speed: Option<u64>,
    user_agent: Option<String>,
    resume: bool,
) -> Result<()> {
    let client = Client::new();
    let res = client.head(url).send().await.context("Failed to send HEAD request")?;
    let content_length = res
        .headers()
        .get(reqwest::header::CONTENT_LENGTH)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<u64>().ok());

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

    if !quiet {
        println!("{} {}", "Downloading:".bold().cyan(), file_name.display().to_string().green());
    }

    let pb = if quiet {
        ProgressBar::hidden()
    } else if let Some(size) = content_length {
        let pb = ProgressBar::new(size);
        pb.set_style(
            ProgressStyle::default_bar()
                .template("{spinner:.green} [{elapsed_precise}] [{bar:40.cyan/blue}] {bytes}/{total_bytes} ({eta})")
                .unwrap()
                .progress_chars("#>-")
        );
        pb
    } else {
        let pb = ProgressBar::new_spinner();
        pb.set_style(
            ProgressStyle::default_bar()
                .template("{spinner:.green} [{elapsed_precise}] {bytes} ({eta})")
                .unwrap()
        );
        pb
    };

    let pb = Arc::new(pb);
    let pb_raw = Arc::into_raw(Arc::clone(&pb)) as usize;

    let url_cstr = CString::new(url)?;
    let path_cstr = CString::new(file_name.to_string_lossy().as_ref())?;
    let url_ptr = url_cstr.as_ptr() as usize;
    let path_ptr = path_cstr.as_ptr() as usize;

    let connections_i32 = connections as i32;
    let max_speed_i64 = max_speed.unwrap_or(0) as i64;
    let ua_cstr = match user_agent {
        Some(ua) => Some(CString::new(ua)?),
        None => None,
    };
    let ua_ptr = ua_cstr.as_ref().map(|c| c.as_ptr() as usize).unwrap_or(0);

    let download_res = tokio::task::spawn_blocking(move || {
        let pb_ptr = pb_raw as *mut c_void;
        let url_raw = url_ptr as *const c_char;
        let path_raw = path_ptr as *const c_char;
        let ua_raw = ua_ptr as *const c_char;
        let ret = unsafe {
            download_http_cpp(
                url_raw,
                path_raw,
                connections_i32,
                quiet,
                Some(rust_progress_callback),
                pb_ptr,
                max_speed_i64,
                ua_raw,
                resume,
            )
        };
        unsafe {
            let _ = Arc::from_raw(pb_raw as *const ProgressBar);
        }
        ret
    })
    .await?;

    if download_res == 0 {
        pb.finish_with_message("Download complete".bold().green().to_string());
        Ok(())
    } else {
        pb.abandon();
        anyhow::bail!("C++ download failed with error code: {}", download_res);
    }
}
