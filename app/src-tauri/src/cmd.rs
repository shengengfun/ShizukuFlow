//! Tauri 命令层：前后端之间唯一的桥。
//!
//! 约定：
//! - 所有返回给前端的错误统一成 `String`（`map_err(|e| e.to_string())`），
//!   这样前端 `invoke` 的 reject 里能直接拿到中文原因去提示。
//! - 工程状态放 `AppState`，前端不直接改文件，一律走命令。

use crate::audio::{AudioCache, AudioMeta, ChannelView, Waveform};
use crate::sf;
use serde_json::json;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::State;

pub struct AppState {
    /// 当前打开的工程。
    pub project: Mutex<Option<sf::Project>>,
    /// 工程文件路径（`.sf` 或 `.sfp`）。
    pub path: Mutex<Option<PathBuf>>,
    /// 资源解析基准目录：`.sf` 是它所在目录，`.sfp` 是解包后的临时目录。
    pub root: Mutex<Option<PathBuf>>,
    /// 用户在设置里手动指定的工具目录（空串表示未指定）。
    pub tools_override: Mutex<String>,
    /// 已加载音轨的波形金字塔缓存（键＝音轨绝对路径）。
    pub audio: AudioCache,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            project: Mutex::new(None),
            path: Mutex::new(None),
            root: Mutex::new(None),
            tools_override: Mutex::new(String::new()),
            audio: AudioCache::new(),
        }
    }
}

// ─────────────────────────── 工程 ───────────────────────────

/// 新建一个空白工程并载入。
#[tauri::command]
pub fn project_new(state: State<'_, AppState>) -> Result<sf::Project, String> {
    let p = sf::Project::new();
    *state.project.lock().unwrap() = Some(p.clone());
    *state.path.lock().unwrap() = None;
    *state.root.lock().unwrap() = None;
    Ok(p)
}

/// 载入一段演示工程（带歌词与镜头），方便第一次打开就看到效果。
#[tauri::command]
pub fn project_demo(state: State<'_, AppState>) -> Result<sf::Project, String> {
    let p = build_demo();
    *state.project.lock().unwrap() = Some(p.clone());
    *state.path.lock().unwrap() = None;
    *state.root.lock().unwrap() = None;
    Ok(p)
}

/// 打开工程（自动识别 `.sf` / `.sfp`）。
#[tauri::command]
pub fn project_open(path: String, state: State<'_, AppState>) -> Result<sf::Project, String> {
    let p = PathBuf::from(&path);
    let (proj, root) = sf::load(&p).map_err(|e| e.to_string())?;

    *state.project.lock().unwrap() = Some(proj.clone());
    *state.path.lock().unwrap() = Some(p);
    *state.root.lock().unwrap() = Some(root);

    Ok(proj)
}

/// 保存到当前路径。没有路径（新建后还没保存过）就报错，让前端去走「另存为」。
#[tauri::command]
pub fn project_save(project: sf::Project, state: State<'_, AppState>) -> Result<(), String> {
    let path = state
        .path
        .lock()
        .unwrap()
        .clone()
        .ok_or_else(|| "还没有指定保存位置，请先另存为".to_string())?;

    let root = state
        .root
        .lock()
        .unwrap()
        .clone()
        .unwrap_or_else(|| path.parent().map(|d| d.to_path_buf()).unwrap_or_default());

    let mut p = project;
    sf::save(&path, &mut p, &root).map_err(|e| e.to_string())?;
    *state.project.lock().unwrap() = Some(p);
    Ok(())
}

/// 另存为。保存后当前路径与工作目录都切到新位置。
#[tauri::command]
pub fn project_save_as(
    path: String,
    project: sf::Project,
    state: State<'_, AppState>,
) -> Result<(), String> {
    let target = PathBuf::from(&path);
    // 另存为时，工作目录跟着换到新文件所在目录
    let root = target
        .parent()
        .map(|d| d.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."));

    let mut p = project;
    sf::save(&target, &mut p, &root).map_err(|e| e.to_string())?;

    *state.project.lock().unwrap() = Some(p);
    *state.path.lock().unwrap() = Some(target);
    *state.root.lock().unwrap() = Some(root);
    Ok(())
}

/// 列出缺失或被改动过的素材。打开工程后前端应立即调一次。
#[tauri::command]
pub fn project_missing_assets(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    let guard = state.project.lock().unwrap();
    let p = guard.as_ref().ok_or_else(|| "没有打开工程".to_string())?;
    let root = state
        .root
        .lock()
        .unwrap()
        .clone()
        .unwrap_or_else(|| PathBuf::from("."));
    Ok(sf::missing_assets(p, &root))
}

#[tauri::command]
pub fn project_duration(state: State<'_, AppState>) -> Result<f64, String> {
    let guard = state.project.lock().unwrap();
    let p = guard.as_ref().ok_or_else(|| "没有打开工程".to_string())?;
    Ok(p.duration())
}

#[tauri::command]
pub fn current_project_dir(state: State<'_, AppState>) -> Option<String> {
    state
        .root
        .lock()
        .unwrap()
        .as_ref()
        .map(|p| p.to_string_lossy().to_string())
}

// ─────────────────────────── 工具链 ───────────────────────────

/// 工具链体检结果。
#[derive(serde::Serialize)]
pub struct ToolsInfo {
    /// 当前生效的工具目录
    pub dir: String,
    /// 来源：`own` / `borrowed` / `custom` / `missing`
    pub origin: &'static str,
    /// 缺失的必需工具（为空即就绪）
    pub missing: Vec<String>,
    /// 找到了多少个可执行文件
    pub found: usize,
}

/// 当前工具目录与体检结果。
///
/// `origin` 会让界面显示「来自岚珠工具箱（借用）」之类的提示 ——
/// 借用只是省一次下载，对方项目删了也不影响我们使用自己的工具。
#[tauri::command]
pub fn tools_dir(state: State<'_, AppState>) -> ToolsInfo {
    let ov = state.tools_override.lock().unwrap().clone();
    let (dir, origin) = crate::tools::resolve_with_origin(&ov);
    let missing = crate::tools::missing_required(&dir);
    let found = crate::tools::list_tools(&dir).len();

    let origin = match origin {
        crate::tools::Origin::Own => "own",
        crate::tools::Origin::Borrowed => "borrowed",
        crate::tools::Origin::Custom => "custom",
        crate::tools::Origin::Missing => "missing",
    };
    ToolsInfo { dir, origin, missing, found }
}

/// 缺失的必需工具（为空即工具链就绪）。
#[tauri::command]
pub fn tools_missing(state: State<'_, AppState>) -> Vec<String> {
    let ov = state.tools_override.lock().unwrap().clone();
    let dir = crate::tools::resolve_tools_dir(&ov);
    crate::tools::missing_required(&dir)
}

/// 列出全部能找到的工具名，给设置页做体检。
#[tauri::command]
pub fn tools_list(state: State<'_, AppState>) -> Vec<String> {
    let ov = state.tools_override.lock().unwrap().clone();
    let dir = crate::tools::resolve_tools_dir(&ov);
    crate::tools::list_tools(&dir)
}

/// 重新扫描工具目录（改了工具目录或新装了工具之后调用）。
#[tauri::command]
pub fn tools_rescan(override_dir: Option<String>, state: State<'_, AppState>) -> Vec<String> {
    if let Some(d) = override_dir {
        *state.tools_override.lock().unwrap() = d;
    }
    crate::tools::reset_cache();
    let ov = state.tools_override.lock().unwrap().clone();
    let dir = crate::tools::resolve_tools_dir(&ov);
    crate::tools::list_tools(&dir)
}

/// 探测硬件编码能力（NVENC / QSV / AMF），给导出设置页用。
#[tauri::command]
pub fn tools_hw_caps(state: State<'_, AppState>) -> crate::tools::HwCaps {
    let ov = state.tools_override.lock().unwrap().clone();
    let dir = crate::tools::resolve_tools_dir(&ov);
    crate::tools::detect_hw_encoders(&dir)
}

// ─────────────────────────── 音频 / 波形 ───────────────────────────

/// 加载音轨并构建波形金字塔。返回元信息（时长等）。
///
/// 内部会缓存 PCM 到磁盘（按文件指纹失效）与内存金字塔，
/// 之后所有 `audio_peaks` 调用都走内存，缩放刮擦零延迟。
#[tauri::command]
pub fn audio_load(audio_path: String, state: State<'_, AppState>) -> Result<AudioMeta, String> {
    let ov = state.tools_override.lock().unwrap().clone();
    let tools_dir = crate::tools::resolve_tools_dir(&ov);

    // 缓存目录放在应用数据目录下，与具体工程解耦
    let cache_dir = crate::app_data_dir()
        .join("audio_cache")
        .to_string_lossy()
        .to_string();

    let loaded = crate::audio::load_with_pyramid(&audio_path, &cache_dir, &tools_dir)?;
    state.audio.insert(audio_path, loaded.pyramid);
    Ok(loaded.meta)
}

/// 取某一段区间的波形峰值（交错 [min,max,...]，归一化到 [-1,1]）。
///
/// `t0`/`t1` 是秒；`width` 是画布像素列数；`view` 是声场视图
///（`mix`/`left`/`right`/`mid`/`side`）。无论缩放到什么比例，
/// 耗时只与 `width` 成正比 —— 这是峰值金字塔带来的「像 Aegisub」的手感。
#[tauri::command]
pub fn audio_peaks(
    audio_path: String,
    t0: f64,
    t1: f64,
    width: u32,
    view: ChannelView,
    state: State<'_, AppState>,
) -> Result<Waveform, String> {
    let pyr = state
        .audio
        .get(&audio_path)
        .ok_or_else(|| "该音轨尚未加载，请先调用 audio_load".to_string())?;
    Ok(pyr.peaks(t0, t1, width, view))
}

// ─────────────────────────── 导出管线 ───────────────────────────
//
// 前端 LUME 内核逐帧渲染 → JPEG/PNG 二进制（Uint8Array）经 IPC 写盘 → ffmpeg 合成 MP4。
// 预览与导出共用同一个 renderFrame（指南 §4.5：禁止两套逻辑）。
// 硬件编码优先（NVENC/QSV/AMF），失败自动回退 libx264。

/// 启动参数（供 CLI 自动导出：`--export <project.sf> <out.mp4>`）。
#[tauri::command]
pub fn app_args() -> Vec<String> {
    std::env::args().collect()
}

/// 建一个干净的帧序列目录，返回绝对路径。
#[tauri::command]
pub fn export_begin(name: String) -> Result<String, String> {
    let safe: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect();
    let dir = std::env::temp_dir()
        .join("ShizukuFlow")
        .join("export")
        .join(format!("{}_{}", safe, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.to_string_lossy().to_string())
}

/// 写一帧（二进制图片字节，前端直接传 `Uint8Array`，Tauri v2 按二进制 IPC 收发，
/// 省去 base64 编码膨胀与字符串解析 —— 每帧少约 33% 传输量，导出更快）。
#[tauri::command]
pub fn export_write(dir: String, name: String, data: Vec<u8>) -> Result<(), String> {
    // 防路径穿越：只允许纯文件名
    if name.contains('/') || name.contains('\\') || name.contains("..") {
        return Err(format!("非法帧文件名：{name}"));
    }
    std::fs::write(std::path::Path::new(&dir).join(name), data).map_err(|e| e.to_string())
}

#[derive(serde::Serialize)]
pub struct MuxResult {
    pub output: String,
    /// 实际使用的视频编码器（可能与请求不同：auto 回退时）
    pub encoder_used: String,
    pub frames: usize,
    pub has_audio: bool,
    /// ffmpeg stderr 尾部（诊断用）
    pub log_tail: String,
}

/// 上次导出成功所用的编码器。放在 app_data_dir 下，重装/更新不会丢。
fn last_encoder_path() -> std::path::PathBuf {
    crate::app_data_dir().join("last_encoder.txt")
}

/// 把帧序列 + 音频合成 MP4。
///
/// `encoder`：auto / software / nvenc / qsv / amf。auto 按探测结果试硬件编码器，
/// 全部失败回退 libx264 —— 用户机器没有 N 卡也能导出，只是慢。
#[tauri::command]
pub fn export_mux(
    frames_dir: String,
    audio_path: Option<String>,
    audio_start: Option<f64>,
    audio_bitrate: Option<u32>,
    out_path: String,
    fps: f64,
    encoder: String,
    crf: u32,
    state: State<'_, AppState>,
) -> Result<MuxResult, String> {
    use std::process::Command;

    let ov = state.tools_override.lock().unwrap().clone();
    let tools_dir = crate::tools::resolve_tools_dir(&ov);
    let ffmpeg = crate::tools::tool(&tools_dir, &crate::tools::bin_name("ffmpeg"));

    // 帧计数（报告用）
    let frames = std::fs::read_dir(&frames_dir)
        .map(|rd| rd.filter_map(|e| e.ok()).filter(|e| e.path().extension().map(|x| x == "png" || x == "jpg" || x == "jpeg").unwrap_or(false)).count())
        .unwrap_or(0);
    if frames == 0 {
        return Err(format!("帧目录为空：{frames_dir}"));
    }
    if let Some(dir) = std::path::Path::new(&out_path).parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }

    // 音频存在性检查（相对路径在前端已解析成绝对路径）
    let audio = audio_path.filter(|p| std::path::Path::new(p).exists());

    // 帧序列：按目录里实际文件的扩展名构造 pattern（jpg 优先——体积小、IPC 快）
    let ext = std::fs::read_dir(&frames_dir)
        .ok()
        .and_then(|mut rd| {
            rd.find_map(|e| {
                e.ok().and_then(|e| {
                    e.path()
                        .extension()
                        .map(|x| x.to_string_lossy().to_lowercase())
                })
            })
        })
        .unwrap_or_else(|| "jpg".into());

    // 候选编码器：auto → 硬件探测序，software → 仅 libx264
    let mut candidates: Vec<(String, Vec<String>)> = Vec::new();
    let vopts_base = |c: &str| -> Vec<String> {
        match c {
            "nvenc" => vec![
                "-c:v".into(), "h264_nvenc".into(),
                "-preset".into(), "p5".into(),
                "-rc".into(), "vbr".into(),
                "-cq".into(), crf.to_string(),
                "-b:v".into(), "0".into(),
            ],
            "qsv" => vec![
                "-c:v".into(), "h264_qsv".into(),
                "-global_quality".into(), crf.to_string(),
            ],
            "amf" => vec![
                "-c:v".into(), "h264_amf".into(),
                "-quality".into(), "quality".into(),
                "-rc".into(), "cqp".into(),
                "-qp_i".into(), crf.to_string(),
                "-qp_p".into(), crf.to_string(),
            ],
            _ => vec![
                "-c:v".into(), "libx264".into(),
                "-preset".into(), "medium".into(),
                "-crf".into(), crf.to_string(),
            ],
        }
    };
    match encoder.as_str() {
        "software" => candidates.push(("libx264".into(), vopts_base("sw"))),
        "nvenc" | "qsv" | "amf" => {
            candidates.push((format!("h264_{encoder}"), vopts_base(&encoder)))
        }
        _ => {
            // auto：硬件优先，软件兜底
            let caps = crate::tools::detect_hw_encoders(&tools_dir);
            if caps.nvenc {
                candidates.push(("h264_nvenc".into(), vopts_base("nvenc")));
            }
            if caps.qsv {
                candidates.push(("h264_qsv".into(), vopts_base("qsv")));
            }
            if caps.amf {
                candidates.push(("h264_amf".into(), vopts_base("amf")));
            }
            candidates.push(("libx264".into(), vopts_base("sw")));

            /*
             * 把上次成功用过的编码器提到最前。
             *
             * 硬件探测只看「ffmpeg 编译时带了这个编码器」，不代表这台机器上真能用 ——
             * 本机就是典型：ffmpeg 编了 ffnvcodec，于是 nvenc 被判定可用，
             * 但运行时 `Cannot load nvcuda.dll`，每次导出都先失败一轮、
             * 往日志糊一屏红色报错，才回退到 QSV。记住上次成功的那个之后，
             * 第二次起直接命中，日志干净，也省掉一轮白跑的编码尝试。
             */
            if let Ok(prev) = std::fs::read_to_string(last_encoder_path()) {
                let prev = prev.trim().to_string();
                if let Some(i) = candidates.iter().position(|(n, _)| *n == prev) {
                    let first = candidates.remove(i);
                    candidates.insert(0, first);
                }
            }
        }
    }

    // 原子落盘：先写 <out>.part.mp4，成功后再 rename 到最终路径。
    // 半途被中断/崩溃时只会留下 .part 文件，绝不会让一个 moov 缺失的坏 MP4
    // 顶着正式文件名冒充成品（曾因此误判"导出成功"）。
    let out_final = std::path::PathBuf::from(&out_path);
    let out_tmp = out_final.with_extension("part.mp4");
    let tmp_path = out_tmp.to_string_lossy().to_string();

    let mut last_log = String::new();
    for (name, vopts) in &candidates {
        // 参数顺序红线：所有 -c:v/-crf 等**输出选项必须放在全部 -i 之后**，
        // 否则会被 ffmpeg 当作下一个输入文件的解码器选项（曾因此报
        // "Unknown decoder 'libx264'"）。
        let mut cmd = Command::new(&ffmpeg);
        cmd.arg("-y")
            .arg("-framerate")
            .arg(fps.to_string())
            .arg("-i")
            .arg(format!("{}\\f%06d.{}", frames_dir, ext));
        if audio.is_some() {
            // 输入 seek：区间导出时音频要从同一时间点起，否则声画错位。
            // 放在 -i 之前是「快速 seek」（关键帧级），对 AAC/MP3 精度约 20ms，
            // 术曲 PV 的区间导出够用；放到 -i 之后会从 0 解码整首，慢得多。
            if let Some(ss) = audio_start.filter(|v| *v > 0.0005) {
                cmd.arg("-ss").arg(format!("{ss:.3}"));
            }
            cmd.arg("-i").arg(audio.as_ref().unwrap());
        }
        cmd.args(vopts)
            .arg("-pix_fmt")
            .arg("yuv420p");
        if audio.is_some() {
            cmd.arg("-c:a").arg("aac")
                .arg("-b:a").arg(format!("{}k", audio_bitrate.unwrap_or(192)));
        }
        cmd.arg("-shortest")
            .arg("-movflags")
            .arg("+faststart")
            .arg(&tmp_path);

        let out = cmd.output().map_err(|e| format!("ffmpeg 启动失败：{e}"))?;
        let log = String::from_utf8_lossy(&out.stderr).to_string();
        last_log = log.clone();

        // 成功判据三个都要过：退出码 0、临时文件存在、体积像样（moov 已写）
        let tmp_size = std::fs::metadata(&tmp_path).map(|m| m.len()).unwrap_or(0);
        if out.status.success() && tmp_size > 64 * 1024 {
            // Windows 上 rename 不覆盖已存在文件，先删目标
            let _ = std::fs::remove_file(&out_final);
            match std::fs::rename(&out_tmp, &out_final) {
                Ok(_) => {
                    // 记住这次成功用的编码器，下次 auto 直接先试它（详见候选排序处注释）
                    let _ = std::fs::write(last_encoder_path(), name);
                    // 结果标记由 Rust 直接落盘：渲染进程若在 mux 后挂掉，
                    // 结果也不会丢（前端那份只是冗余备份）。
                    let _ = std::fs::write(
                        format!("{}.flag", out_path),
                        format!("OK\tencoder={} frames={}\n", name, frames),
                    );
                    return Ok(MuxResult {
                        output: out_path,
                        encoder_used: name.clone(),
                        frames,
                        has_audio: audio.is_some(),
                        log_tail: log.chars().rev().take(1500).collect::<Vec<_>>().into_iter().rev().collect(),
                    });
                }
                Err(e) => {
                    last_log = format!("临时文件改名失败：{e}");
                    let _ = std::fs::remove_file(&out_tmp);
                    continue;
                }
            }
        }
        let _ = std::fs::remove_file(&out_tmp);
        eprintln!("[export_mux] 编码器 {name} 失败，尝试下一个：\n{log}");
    }

    // 全部编码器失败：也由 Rust 落盘失败标记（截断日志，避免 flag 巨大）
    let brief: String = last_log.lines().filter(|l| !l.trim().is_empty()).take(3).collect::<Vec<_>>().join(" | ");
    let _ = std::fs::write(format!("{}.flag", out_path), format!("FAIL\t{}\n", &brief[..brief.len().min(300)]));
    Err(format!("所有编码器均失败。最后日志：\n{last_log}"))
}

/// 写一个标记文件（自动导出流程报告成功/失败用，便于脚本判断结果）。
#[tauri::command]
pub fn export_flag(path: String, ok: bool, message: String) -> Result<(), String> {
    std::fs::write(
        &path,
        format!("{}\t{}\n", if ok { "OK" } else { "FAIL" }, message),
    )
    .map_err(|e| e.to_string())
}

// ─────────────────────────── 演示工程 ───────────────────────────

/// 构造一段演示工程：4 行歌词 + 对应镜头，覆盖 0–12s。
///
/// 用作「示例工程」按钮的数据源，也当渲染内核的回归 fixture。
fn build_demo() -> sf::Project {
    let mut p = sf::Project::new();
    p.meta.title = "残光（演示）".into();
    p.meta.artist = "ShizukuFlow".into();
    p.timing.bpm = 140.0;

    // 每行歌词：文字 + 起止时间。逐字音节按时间均匀切分。
    let lines: &[(&str, f64, f64)] = &[
        ("我还记得黎明的颜色", 0.0, 3.0),
        ("沉入海里的那束光", 3.0, 6.0),
        ("像被风吹散的星屑", 6.0, 9.0),
        ("在指尖悄然消散", 9.0, 12.0),
    ];

    for (i, &(text, start, end)) in lines.iter().enumerate() {
        let id = format!("L{:03}", i + 1);
        // 逐字：每个汉字一个音节，均匀铺在 start..end
        let chars: Vec<char> = text.chars().collect();
        let n = chars.len().max(1) as f64;
        let mut syllables = Vec::new();
        for (j, ch) in chars.iter().enumerate() {
            let s = start + (j as f64) * (end - start) / n;
            let e = start + ((j + 1) as f64) * (end - start) / n;
            syllables.push(sf::Syllable {
                text: ch.to_string(),
                start: s,
                end: e,
            });
        }
        p.lyrics.push(sf::Lyric {
            id: id.clone(),
            start,
            end,
            text: text.to_string(),
            syllables,
            style: "main".into(),
        });

        // 每个主镜头（track 0）带一层渐变背景部件 —— 验证「空间层数量不封顶」
        // 与 background 部件渲染。颜色随段落轻微变化，保持术曲 PV 的暗调。
        let bg_pairs: [(&str, &str); 4] = [
            ("#0b1020", "#241026"),
            ("#101020", "#2a0f1e"),
            ("#0a1422", "#1e1330"),
            ("#141018", "#301a0f"),
        ];
        let (c1, c2) = bg_pairs[i % bg_pairs.len()];
        p.shots.push(sf::Shot {
            id: format!("S{:03}", i + 1),
            start,
            end,
            track: 0,
            lyric_refs: vec![id],
            layers: sf::Layers {
                background: vec![sf::PartRef {
                    part: "background".into(),
                    params: json!({ "kind": "gradient", "color": c1, "color2": c2, "angle": 110 + i as i64 * 20 }),
                }],
                text: vec![],
                decor: vec![],
                overlay: vec![],
            },
            motion: sf::Motion::default(),
            transition: None,
            seed: 1000 + i as u64 * 7919,
            locked: false,
        });
    }

    // 常驻覆盖轨（track 1）：盖在所有主镜头之上，演示「时间轨数量不封顶」。
    // - 渐晕（overlay 部件）让画面更有电影感；
    // - 标题「残光」只在 0–3s 显示（showStart/showEnd 时间窗）；
    // - 底部署名常驻整片。
    p.shots.push(sf::Shot {
        id: "S900".into(),
        start: 0.0,
        end: 12.0,
        track: 1,
        lyric_refs: vec![],
        layers: sf::Layers {
            background: vec![],
            text: vec![
                sf::PartRef {
                    part: "text".into(),
                    params: json!({
                        "text": "残光",
                        "x": 0.5, "y": 0.34,
                        "size": 180, "weight": 900,
                        "fill": "#ffffff",
                        "stroke": { "color": "#000000", "width": 6 },
                        "shadow": { "blur": 18, "color": "#000000aa", "offset": [0, 6] },
                        "showStart": 0.0, "showEnd": 3.0
                    }),
                },
                sf::PartRef {
                    part: "text".into(),
                    params: json!({
                        "text": "ShizukuFlow · Demo",
                        "x": 0.5, "y": 0.94,
                        "size": 30, "weight": 600,
                        "fill": "rgba(255,255,255,0.65)"
                    }),
                },
            ],
            decor: vec![],
            overlay: vec![sf::PartRef {
                part: "overlay".into(),
                params: json!({ "kind": "vignette", "color": "rgba(0,0,0,0.5)", "inner": "rgba(0,0,0,0)" }),
            }],
        },
        motion: sf::Motion::default(),
        transition: None,
        seed: 7,
        locked: false,
    });

    p
}

#[cfg(test)]
mod tests {
    use super::build_demo;

    #[test]
    fn demo_has_four_lines_with_full_coverage() {
        let p = build_demo();
        assert_eq!(p.lyrics.len(), 4);
        // 4 个主镜头 + 1 个常驻覆盖轨（track 1）
        assert_eq!(p.shots.len(), 5);
        // track 1 的覆盖轨必须存在且跨满整片
        let overlay = p.shots.iter().find(|s| s.id == "S900").expect("应有常驻覆盖轨");
        assert_eq!(overlay.track, 1);
        assert!((overlay.start - 0.0).abs() < 1e-6);
        assert!((overlay.end - 12.0).abs() < 1e-6);
        for l in &p.lyrics {
            assert!(!l.syllables.is_empty(), "{} 必须有逐字数据", l.id);
            let first = l.syllables.first().unwrap();
            let last = l.syllables.last().unwrap();
            assert!((first.start - l.start).abs() < 1e-6, "首字起点要对齐行起点");
            assert!((last.end - l.end).abs() < 1e-6, "末字终点要对齐行终点");
        }
        // 时长应取最后一行终点 12s
        assert!((p.duration() - 12.0).abs() < 1e-6);
    }

    #[test]
    fn demo_seeds_are_distinct_and_nonzero() {
        let p = build_demo();
        let seeds: std::collections::HashSet<u64> =
            p.shots.iter().map(|s| s.seed).collect();
        assert_eq!(seeds.len(), p.shots.len(), "各镜头种子必须不同（可复现前提）");
        assert!(p.shots.iter().all(|s| s.seed != 0));
    }
}
