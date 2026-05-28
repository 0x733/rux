fn main() {
    println!("cargo:rerun-if-changed=src/http_downloader.cpp");
    cc::Build::new()
        .cpp(true)
        .file("src/http_downloader.cpp")
        .flag("-std=c++17")
        .compile("http_downloader_cpp");
    println!("cargo:rustc-link-lib=curl");
}
