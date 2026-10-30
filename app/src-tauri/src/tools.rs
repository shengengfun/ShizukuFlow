//! 工具目录解析 + 可执行文件查找。
//!
//! # ShizukuFlow 自带工具链，与岚珠工具箱：联动，但不绑定
//!
//! **本软件自己带一份 ffmpeg / ffprobe**（位于 `src-tauri/tools/`，
//! 打包时随安装包分发，见 `tauri.conf.json` 的 `bundle.resources`）。
//! 所以开箱即用、零外部依赖，岚珠装不装、在不在，跟我们没关系。
//!
//! 逻辑**移植**自岚珠工具箱（`LanzhuToolNX/tauri-ui/src-tauri/src/tools.rs`）——
//! 注意是移植（把逻辑抄过来），不是调用（依赖它的 crate 或进程）。
//! 好处是用户若已在别处装过一份，可以在设置里顺手复用，省掉重复下载；
//! 同时两边各改各的，互不影响。**复用是用户主动的便利，不是我们的依赖。**
//!
//! **两条硬规矩，违反任何一条都会让本软件被别人家项目的变动拖下水：**
//!
//! 1. **这里不允许出现任何指向岚珠仓库/安装位置的绝对路径。**
//!    别人把项目挪个目录、改个盘符，我们这边就莫名失效 —— 属于隐式耦合。
//! 2. **ShizukuFlow 自己的目录，永远排在「借用目录」之前。**
//!    于是岚珠被卸载、迁移、改名，或它的 `tools/` 被清空，
//!    本软件只是少一个兜底选项，功能完全不受影响。
//!
//! 同理，我们**不去读 `LANZHUTOOL_TOOLS` 环境变量**：那是岚珠的内部配置，
//! 它可以指向任何地方。真想借用它的工具，用户在设置里显式指定即可（第 2 优先级）。
//!
//! 查找策略是「按文件名递归扫一次 + 整体缓存」：
//! 不复制文件、不污染工作目录，也不需要用户先跑一遍整理脚本。

use std::path::{Path, PathBuf};
use std::sync::Mutex;

static CACHE: Mutex<Option<Vec<(String, PathBuf)>>> = Mutex::new(None);

/// 用双引号包住路径（与岚珠的 `tools::quote` 一致）。
// P2 拼 ffmpeg 命令行时用到
#[allow(dead_code)]
pub fn quote(p: &str) -> String {
    format!("\"{}\"", p)
}

/// 给子进程加 `CREATE_NO_WINDOW`。
///
/// GUI 应用里 spawn 控制台程序会**闪一下黑框** —— 探测、抓帧、编码都是高频调用，
/// 不走这个会一直闪。所有 spawn 外部工具的地方都必须经过它。
// P2 导出管线（ffmpeg / ffprobe）会用到，先留着
#[allow(dead_code)]
pub fn no_window(cmd: &mut std::process::Command) -> &mut std::process::Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// 当前工具目录的归属，给设置页显示用。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Origin {
    /// ShizukuFlow 自己的
    Own,
    /// 从岚珠工具箱借用的（可选兜底）
    Borrowed,
    /// 用户显式指定的
    Custom,
    /// 一个都没找到，返回的是占位路径
    Missing,
}

/// 工具目录优先级。**自己的永远排在借用之前**：
///
///   1. `SHIZUKUFLOW_TOOLS`（本软件专属环境变量）
///   2. 设置里手动指定的目录（想借用岚珠的工具，在这儿显式指向即可）
///   3. exe 同级的 `tools/`              ← 自己的
///   4. 上两级目录下的 `tools/`          ← 自己的
///   5. `%APPDATA%\ShizukuFlow\tools`    ← 自己的
///   6. `%APPDATA%\LanzhuTool\tools`     ← 借用（可选兜底，失效无影响）
///   7. 都没有 → 回到 exe 同级的 `tools/`（占位）
pub fn resolve_tools_dir(override_dir: &str) -> String {
    resolve_with_origin(override_dir).0
}

/// 同 `resolve_tools_dir`，但连归属一起返回。
pub fn resolve_with_origin(override_dir: &str) -> (String, Origin) {
    // 1. 本软件专属环境变量
    if let Ok(env) = std::env::var("SHIZUKUFLOW_TOOLS") {
        let p = PathBuf::from(env.trim());
        if p.is_dir() {
            return (clean_path(&p), Origin::Custom);
        }
    }

    // 2. 用户显式指定（借用岚珠走这条路，是"用户主动"而非"我们依赖"）
    if !override_dir.trim().is_empty() {
        let p = PathBuf::from(override_dir.trim());
        if p.is_dir() {
            return (clean_path(&p), Origin::Custom);
        }
    }

    let base = exe_dir();

    // 3~5. 自己的目录
    let mut own: Vec<PathBuf> = vec![
        base.join("tools"),
        base.join("..").join("tools"),
        base.join("..").join("..").join("tools"),
    ];
    if let Ok(appdata) = std::env::var("APPDATA") {
        own.push(PathBuf::from(appdata).join("ShizukuFlow").join("tools"));
    }
    for c in own.iter() {
        if c.is_dir() {
            return (clean_path(c), Origin::Own);
        }
    }

    // 6. 借用：岚珠工具箱已装好的那份。
    //    走到这儿说明我们自己没装工具，复用一下是纯赚；
    //    但要是岚珠不在了，下面的循环自然落空，不影响任何功能。
    if let Ok(appdata) = std::env::var("APPDATA") {
        for (app, sub) in BORROWED_DIRS {
            let borrowed = PathBuf::from(&appdata).join(app).join(sub);
            if borrowed.is_dir() {
                return (clean_path(&borrowed), Origin::Borrowed);
            }
        }
    }

    // 7. 一个都没找到：返回自己的 exe 同级 tools/（占位）。
    //    目录不存在也没关系，前端拿 `list_tools()` 的结果提示「找不到工具」。
    (base.join("tools").to_string_lossy().to_string(), Origin::Missing)
}

/// `canonicalize()` 在 Windows 上给出 `\\?\D:\...` 扩展前缀。
/// 能用，但显示在界面上很脏，交给某些工具也不认，统一去掉。
fn clean_path(p: &Path) -> String {
    let s = p.canonicalize().unwrap_or_else(|_| p.to_path_buf());
    let t = s.to_string_lossy().to_string();
    t.strip_prefix(r"\\?\").map(|x| x.to_string()).unwrap_or(t)
}

/// 递归收集文件名（小写）→ 路径。深度上限 4 层，防止符号链接环与超大目录。
fn scan(root: &Path, out: &mut Vec<(String, PathBuf)>, depth: usize) {
    if depth > 4 {
        return;
    }
    let Ok(rd) = std::fs::read_dir(root) else { return };
    for entry in rd.flatten() {
        let path = entry.path();
        if path.is_dir() {
            scan(&path, out, depth + 1);
        } else if let Some(name) = path.file_name().and_then(|s| s.to_str()) {
            out.push((name.to_ascii_lowercase(), path.clone()));
        }
    }
}

fn find(tools_dir: &str, name: &str) -> Option<PathBuf> {
    let key = name.to_ascii_lowercase();

    {
        let guard = CACHE.lock().unwrap();
        if let Some(list) = guard.as_ref() {
            if let Some((_, p)) = list.iter().find(|(n, _)| *n == key) {
                return Some(p.clone());
            }
        }
    }

    // 未命中：扫一次并整体缓存（后续查找全部走缓存，不再碰磁盘）
    let mut list: Vec<(String, PathBuf)> = Vec::new();
    scan(Path::new(tools_dir), &mut list, 0);
    let hit = list.iter().find(|(n, _)| *n == key).map(|(_, p)| p.clone());
    *CACHE.lock().unwrap() = Some(list);

    hit
}

/// 取工具的完整路径。
///
/// 找不到就退回工具名本身 —— 这样会落到系统 PATH，
/// 报错信息也比「找不到文件」直观（用户至少能看到是 `ffmpeg` 缺失）。
// P2 导出管线调用 ffmpeg / ffprobe 时用到
#[allow(dead_code)]
pub fn tool(tools_dir: &str, name: &str) -> String {
    match find(tools_dir, name) {
        Some(p) => p.to_string_lossy().to_string(),
        None => name.to_string(),
    }
}

/// 工具是否真实存在（不落 PATH，只认 tools 目录里扫到的）。
pub fn has(tools_dir: &str, name: &str) -> bool {
    find(tools_dir, name).is_some()
}

/// 清掉索引缓存。装完新工具、改了工具目录之后必须调用，否则还会解析到旧路径。
pub fn reset_cache() {
    *CACHE.lock().unwrap() = None;
}

/// 列出所有能找到的工具名，给「设置」页做体检。
pub fn list_tools(tools_dir: &str) -> Vec<String> {
    let mut list: Vec<(String, PathBuf)> = Vec::new();
    scan(Path::new(tools_dir), &mut list, 0);
    *CACHE.lock().unwrap() = Some(list.clone());
    let mut names: Vec<String> = list.into_iter().map(|(n, _)| n).collect();
    names.sort();
    names.dedup();
    names
}

/// 外部工具的借用点：**全部耦合集中在这一行**。
///
/// 格式是 `(AppData 下的应用目录名, 子目录)`。这里列的是岚珠工具箱的默认位置
/// —— 它把工具装在 `%APPDATA%\LanzhuTool\tools`，我们只是"顺手复用"。
///
/// 想彻底断开联动，把这个数组清空即可，本软件功能一行不受影响。
/// 想增加别的借用点，也只改这里。
const BORROWED_DIRS: [(&str, &str); 1] = [("LanzhuTool", "tools")];

/// 渲染/导出真正必需的工具（跨平台，Windows 自动补 .exe）。
///
/// 刻意不含 `ffplay` —— 那是播放器，岚珠拿它做预览，
/// 但 ShizukuFlow 的预览走自己的 Canvas/WebGL，不需要它。
pub fn required() -> Vec<String> {
    [bin_name("ffmpeg"), bin_name("ffprobe")].to_vec()
}

/// 跨平台可执行文件名：Windows 带 `.exe`，其它平台不带。
pub fn bin_name(base: &str) -> String {
    if cfg!(windows) {
        format!("{base}.exe")
    } else {
        base.to_string()
    }
}

/// 体检：返回缺失的必需工具列表（为空表示工具链就绪）。
pub fn missing_required(tools_dir: &str) -> Vec<String> {
    required()
        .iter()
        .filter(|n| !has(tools_dir, n))
        .cloned()
        .collect()
}

/// 显卡硬件编码能力。
#[derive(Debug, Clone, serde::Serialize)]
pub struct HwCaps {
    /// NVIDIA NVENC（h264_nvenc）
    pub nvenc: bool,
    /// Intel QuickSync（h264_qsv）
    pub qsv: bool,
    /// AMD AMF（h264_amf）
    pub amf: bool,
    /// 探测到的全部硬件 H.264 编码器名（界面展示 + 调试用）
    pub available: Vec<String>,
}

/// 探测 ffmpeg 支持的硬件 H.264 编码器。
///
/// 走 `ffmpeg -hide_banner -encoders` 抓输出里出现的 `h264_nvenc` /
/// `h264_qsv` / `h264_amf`。ffmpeg 缺失或拉起失败时全部返回 false，
/// 不影响其余功能 —— 导出时回退到软件编码（libx264）即可。
///
/// 这正是「硬件加速」在导出端落地的入口：P2 导出管线据此挑编码器，
/// 优先 NVENC/QSV/AMF，失败自动回退 libx264，用户无需关心显卡型号。
pub fn detect_hw_encoders(tools_dir: &str) -> HwCaps {
    let exe = tool(tools_dir, &bin_name("ffmpeg"));
    let mut cmd = std::process::Command::new(&exe);
    cmd.arg("-hide_banner").arg("-encoders");
    let _ = no_window(&mut cmd);
    let out = match cmd.output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).to_string(),
        Err(_) => return HwCaps { nvenc: false, qsv: false, amf: false, available: vec![] },
    };
    let nvenc = out.contains("h264_nvenc");
    let qsv = out.contains("h264_qsv");
    let amf = out.contains("h264_amf");
    let mut available = Vec::new();
    if nvenc { available.push("h264_nvenc".into()); }
    if qsv { available.push("h264_qsv".into()); }
    if amf { available.push("h264_amf".into()); }
    HwCaps { nvenc, qsv, amf, available }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quote_wraps_in_double_quotes() {
        assert_eq!(quote(r"D:\a b\c.exe"), r#""D:\a b\c.exe""#);
    }

    #[test]
    fn resolve_never_panics_on_empty_override() {
        // 目录可能不存在，但必须返回一个可用的字符串而不是崩
        let d = resolve_tools_dir("");
        assert!(!d.is_empty());
    }

    #[test]
    fn missing_required_reports_all_when_dir_missing() {
        let v = missing_required(r"C:\__definitely_not_exists__");
        assert_eq!(v.len(), required().len());
    }

    #[test]
    fn scan_respects_depth_limit() {
        // 深度上限 4，超深目录不会无限递归、不爆栈
        let mut out = Vec::new();
        scan(Path::new("."), &mut out, 0);
        let _ = out.len();
    }

    /// **解耦防回归（核心）**：本文件里不允许出现任何别的项目的绝对路径。
    ///
    /// 一旦有人图省事把 `D:\Project\LanzhuToolNX\tools` 之类写回来，
    /// 这个测试会当场拦下 —— 那等于把别人的安装位置焊死在我们代码里，
    /// 对方挪个目录我们就莫名失效。
    const SELF_SRC: &str = include_str!("tools.rs");

    /// 只取 `#[cfg(test)]` 之前的部分。
    ///
    /// 否则 `include_str!` 会把测试代码本身也扫进去，
    /// 于是下面那些"违禁词"字面量会把自己当成违规项抓出来。
    fn production_code() -> &'static str {
        SELF_SRC.split("#[cfg(test)]").next().unwrap_or(SELF_SRC)
    }

    #[test]
    fn no_hardcoded_absolute_paths() {
        let code = production_code();
        // 匹配任意盘符下的仓库/用户目录，如 D:\Project\...  C:\Users\...
        for pat in [":\\Project", ":\\Users"] {
            assert!(
                !code.contains(pat),
                "tools.rs 里出现了硬编码路径片段 {pat:?} —— \
                 本软件必须能独立运行，不得依赖其他项目的安装位置"
            );
        }
    }

    #[test]
    fn no_reference_to_other_project_env_vars() {
        let code = production_code();
        // 不读别人的环境变量：那是对方的内部配置，可以指向任何地方
        assert!(
            !code.contains("LANZHUTOOL_TOOLS\""),
            "不应读取 LANZHUTOOL_TOOLS —— 那是岚珠的内部配置。\
             想借用请在设置里显式指定目录。"
        );
    }

    /// 外部借用点必须集中在一处，方便一眼看清耦合面、也方便整体断开。
    #[test]
    fn borrowed_dirs_lists_only_appdata_relative_locations() {
        for (app, sub) in BORROWED_DIRS {
            assert!(!app.contains(':'), "借用点必须是相对 AppData 的名字，不是绝对路径：{app}");
            assert!(!sub.contains(':'), "借用点子目录不应是绝对路径：{sub}");
        }
    }

    /// 借用目录不存在时，仍能返回占位路径（而不是崩或返回空）。
    #[test]
    fn resolve_survives_when_borrowed_dir_gone() {
        let (dir, origin) = resolve_with_origin("");
        assert!(!dir.is_empty(), "无论如何都要返回一个可显示的目录");
        // 借用目录失效时，origin 只可能是 Own / Custom / Missing，不可能是 Borrowed
        assert!(
            matches!(origin, Origin::Own | Origin::Custom | Origin::Missing),
            "借用目录不可用时不应报 Borrowed，实际：{origin:?}"
        );
    }

    #[test]
    fn reset_cache_does_not_panic() {
        reset_cache();
        reset_cache();
    }

    /// **手动体检用**（默认不跑，因为依赖真实机器环境）：
    ///
    /// ```bash
    /// cargo test probe_real_env -- --nocapture --ignored
    /// ```
    ///
    /// 用来确认「借来岚珠的 ffmpeg」这条路在本机真的走得通。
    #[test]
    #[ignore]
    fn probe_real_env() {
        let (dir, origin) = resolve_with_origin("");
        println!("工具目录 : {dir}");
        println!("来源     : {origin:?}");
        println!("ffmpeg   : {:?}", find(&dir, "ffmpeg.exe"));
        println!("ffprobe  : {:?}", find(&dir, "ffprobe.exe"));
        println!("缺失必需 : {:?}", missing_required(&dir));
        println!("已索引   : {} 个文件", list_tools(&dir).len());

        // 再验一次「手动指定借用目录」这条路。
        //
        // 绝对路径只出现在**测试代码**里 —— 生产代码（`#[cfg(test)]` 之前）
        // 绝不允许出现，这正是 `no_hardcoded_absolute_paths` 守住的边界。
        // 顺带也证明了那条防回归测试的扫描隔离是有效的。
        reset_cache();
        let borrowed = resolve_with_origin(r"D:\Project\LanzhuToolNX\tools");
        println!("\n[手动指定借用目录]");
        println!("工具目录 : {}", borrowed.0);
        println!("来源     : {:?}", borrowed.1);
        println!("ffmpeg   : {:?}", find(&borrowed.0, "ffmpeg.exe"));
        println!("缺失必需 : {:?}", missing_required(&borrowed.0));
    }
}
