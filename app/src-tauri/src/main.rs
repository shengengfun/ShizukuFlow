// 发布版不弹控制台窗口
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod audio;
mod cmd;
mod sf;
mod tools;

use cmd::AppState;

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            // 工程
            cmd::project_new,
            cmd::project_open,
            cmd::project_demo,
            cmd::project_save,
            cmd::project_save_as,
            cmd::project_missing_assets,
            cmd::project_duration,
            cmd::current_project_dir,
            // 工具链
            cmd::tools_dir,
            cmd::tools_missing,
            cmd::tools_list,
            cmd::tools_rescan,
            cmd::tools_hw_caps,
            // 音频 / 波形
            cmd::audio_load,
            cmd::audio_peaks,
            // 导出管线
            cmd::app_args,
            cmd::export_begin,
            cmd::export_write,
            cmd::export_mux,
            cmd::export_flag,
        ])
        .run(tauri::generate_context!())
        .expect("ShizukuFlow 启动失败");
}

/// 应用数据目录：`%APPDATA%\ShizukuFlow`，用于放设置与自动保存快照。
#[allow(dead_code)]
pub fn app_data_dir() -> std::path::PathBuf {
    let base = std::env::var("APPDATA")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    let d = base.join("ShizukuFlow");
    let _ = std::fs::create_dir_all(&d);
    d
}

/// 自动保存目录。崩溃恢复靠它。
#[allow(dead_code)]
pub fn autosave_dir() -> std::path::PathBuf {
    let d = app_data_dir().join("autosave");
    let _ = std::fs::create_dir_all(&d);
    d
}
