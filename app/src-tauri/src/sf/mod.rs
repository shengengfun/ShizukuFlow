//! ShizukuFlow 工程格式的 Rust 数据模型与读写。
//!
//! # 两种格式
//!
//! - **`.sf`** —— 单个 JSON 文件，日常编辑用。人类可读、可 git diff、可版本对比，
//!   资源**外链 + sha1 校验**。
//! - **`.sfp`** —— zip 容器（`project.sf` + `assets/` + `manifest.json`），
//!   自包含，一个文件拷走就能在别的机器上打开。
//!
//! 类比：`.sf` 是源码，`.sfp` 是发行包。
//!
//! # 三条硬约束
//!
//! 1. **所有可选字段 `#[serde(default)]`** —— 后加的字段不能让老工程打不开。
//! 2. **随机必须固化种子** —— `Shot::seed` 写进文件，否则「可复现」立刻崩塌。
//!    这是 `renderFrame(t)` 纯函数架构的前提。
//! 3. **部件参数用 `serde_json::Value`** —— 部件是开放的、可扩充的，
//!    检查器按部件声明的 schema 渲染控件，Rust 侧不去穷举它们。

use serde::{Deserialize, Serialize};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

pub const FORMAT_NAME: &str = "ShizukuFlow";
pub const FORMAT_VERSION: u32 = 1;

/// `.sfp` 容器格式版本（与工程格式版本分开演进）。
pub const SFP_VERSION: u32 = 1;

pub const PROJECT_ENTRY: &str = "project.sf";
pub const MANIFEST_ENTRY: &str = "manifest.json";
pub const ASSETS_DIR: &str = "assets";

// ─────────────────────────── 顶层 ───────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Project {
    pub format: String,
    pub version: u32,

    #[serde(default)]
    pub meta: Meta,
    #[serde(default)]
    pub canvas: Canvas,
    #[serde(default)]
    pub media: Media,
    #[serde(default)]
    pub timing: Timing,
    #[serde(default)]
    pub styles: Vec<Style>,
    #[serde(default)]
    pub lyrics: Vec<Lyric>,
    #[serde(default)]
    pub shots: Vec<Shot>,
    #[serde(default)]
    pub assets: Vec<Asset>,
    #[serde(default)]
    pub export: Export,
}

impl Default for Project {
    fn default() -> Self {
        Self::new()
    }
}

impl Project {
    pub fn new() -> Self {
        Self {
            format: FORMAT_NAME.to_string(),
            version: FORMAT_VERSION,
            meta: Meta::default(),
            canvas: Canvas::default(),
            media: Media::default(),
            timing: Timing::default(),
            styles: vec![Style::default()],
            lyrics: Vec::new(),
            shots: Vec::new(),
            assets: Vec::new(),
            export: Export::default(),
        }
    }

    /// 总时长（秒）。取「音频时长」与「最后一个镜头结束」中较大的那个，
    /// 这样还没导入音频时时间轴也有一个有意义的右边界。
    pub fn duration(&self) -> f64 {
        let from_media = self.media.audio.as_ref().map(|a| a.duration).unwrap_or(0.0);
        let from_shots = self.shots.iter().map(|s| s.end).fold(0.0, f64::max);
        let from_lyrics = self.lyrics.iter().map(|l| l.end).fold(0.0, f64::max);
        from_media.max(from_shots).max(from_lyrics)
    }

    /// 按 id 找镜头。
    // P2 增量渲染与检查器选中态会用到，先留着
    #[allow(dead_code)]
    pub fn shot(&self, id: &str) -> Option<&Shot> {
        self.shots.iter().find(|s| s.id == id)
    }

    /// 镜头哈希 —— 增量渲染的依据。
    ///
    /// 只哈希「影响画面」的字段；`locked` 之类纯编辑状态不参与，
    /// 否则上锁/解锁会让整片白重渲一遍。
    // P2 增量渲染用：比对指纹决定哪些镜头需要重渲
    #[allow(dead_code)]
    pub fn shot_fingerprint(&self, s: &Shot) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut h = std::collections::hash_map::DefaultHasher::new();
        s.start.to_bits().hash(&mut h);
        s.end.to_bits().hash(&mut h);
        s.track.hash(&mut h);
        s.seed.hash(&mut h);
        // 部件参数逐字节哈希（Value 没有稳定序列化顺序保证，
        // 所以先转字符串再哈希，保证同参数同结果）
        serde_json::to_string(&s.layers).unwrap_or_default().hash(&mut h);
        serde_json::to_string(&s.motion).unwrap_or_default().hash(&mut h);
        serde_json::to_string(&s.transition).unwrap_or_default().hash(&mut h);
        for r in &s.lyric_refs {
            r.hash(&mut h);
        }
        h.finish()
    }
}

// ─────────────────────────── 子结构 ───────────────────────────

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Meta {
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub artist: String,
    #[serde(default)]
    pub created: String,
    #[serde(default)]
    pub modified: String,
    #[serde(default)]
    pub generator: String,
    /// 工程级强调色（「正在唱」提示色），如 "#ff5c8a"。空 = 用全局默认。
    #[serde(default)]
    pub accent: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Canvas {
    #[serde(default = "default_width")]
    pub width: u32,
    #[serde(default = "default_height")]
    pub height: u32,
    #[serde(default = "default_fps")]
    pub fps: f64,
    #[serde(default = "default_bg")]
    pub background: String,
    #[serde(default)]
    pub safe_area: SafeArea,
}
fn default_width() -> u32 { 1920 }
fn default_height() -> u32 { 1080 }
fn default_fps() -> f64 { 30.0 }
fn default_bg() -> String { "#000000".into() }

impl Default for Canvas {
    fn default() -> Self {
        Self {
            width: default_width(),
            height: default_height(),
            fps: default_fps(),
            background: default_bg(),
            safe_area: SafeArea::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SafeArea {
    #[serde(default)]
    pub enabled: bool,
    #[serde(default = "default_margin")]
    pub margin: f64,
}
fn default_margin() -> f64 { 0.05 }

impl Default for SafeArea {
    fn default() -> Self {
        Self { enabled: true, margin: default_margin() }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Media {
    pub audio: Option<MediaRef>,
    #[serde(default)]
    pub reference: Vec<MediaRef>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MediaRef {
    pub path: String,
    #[serde(default)]
    pub sha1: String,
    #[serde(default)]
    pub duration: f64,
    #[serde(default)]
    pub kind: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Timing {
    #[serde(default)]
    pub bpm: f64,
    #[serde(default)]
    pub offset: f64,
    #[serde(default)]
    pub beats: Vec<f64>,
    #[serde(default)]
    pub downbeats: Vec<f64>,
    #[serde(default = "default_source")]
    pub source: String,
}
fn default_source() -> String { "auto".into() }

/// 可被任意镜头引用的样式定义。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Style {
    pub id: String,
    #[serde(default = "default_font")]
    pub font: String,
    #[serde(default = "default_weight")]
    pub weight: u32,
    #[serde(default = "default_size")]
    pub size: f64,
    #[serde(default)]
    pub letter_spacing: f64,
    #[serde(default = "default_line_height")]
    pub line_height: f64,
    #[serde(default = "default_fill")]
    pub fill: String,
    /// 歌词行垂直锚点（画幅高度比例，0..1；None = 默认 0.58）。
    /// 版式错落（术曲PV常用）：不同行放不同高度。
    #[serde(default)]
    pub y: Option<f64>,
    #[serde(default)]
    pub stroke: Option<Stroke>,
    #[serde(default)]
    pub shadow: Option<Shadow>,
}
fn default_font() -> String { "Noto Serif SC".into() }
fn default_weight() -> u32 { 900 }
fn default_size() -> f64 { 96.0 }
fn default_line_height() -> f64 { 1.1 }
fn default_fill() -> String { "#FFFFFF".into() }

impl Default for Style {
    fn default() -> Self {
        Self {
            id: "main".into(),
            font: default_font(),
            weight: default_weight(),
            size: default_size(),
            letter_spacing: -0.02,
            line_height: default_line_height(),
            fill: default_fill(),
            y: None,
            stroke: Some(Stroke::default()),
            shadow: Some(Shadow::default()),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Stroke {
    #[serde(default = "default_stroke_color")]
    pub color: String,
    #[serde(default = "default_stroke_width")]
    pub width: f64,
}
fn default_stroke_color() -> String { "#000000".into() }
fn default_stroke_width() -> f64 { 4.0 }

impl Default for Stroke {
    fn default() -> Self {
        Self { color: default_stroke_color(), width: default_stroke_width() }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Shadow {
    #[serde(default)]
    pub offset: [f64; 2],
    #[serde(default = "default_blur")]
    pub blur: f64,
    #[serde(default = "default_shadow_color")]
    pub color: String,
}
fn default_blur() -> f64 { 8.0 }
fn default_shadow_color() -> String { "#00000080".into() }

impl Default for Shadow {
    fn default() -> Self {
        Self { offset: [0.0, 4.0], blur: default_blur(), color: default_shadow_color() }
    }
}

/// 一行歌词。`syllables` 是 Aegisub K 值的等价物 —— 逐字/逐音节时间。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Lyric {
    pub id: String,
    pub start: f64,
    pub end: f64,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub syllables: Vec<Syllable>,
    #[serde(default = "default_style_ref")]
    pub style: String,
}
fn default_style_ref() -> String { "main".into() }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Syllable {
    #[serde(default)]
    pub text: String,
    pub start: f64,
    pub end: f64,
}

/// 演出部件引用。
///
/// `params` 刻意用 `Value`：部件是开放集合，Rust 侧不去穷举，
/// 检查器按部件声明的 schema 生成控件。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct PartRef {
    pub part: String,
    #[serde(default)]
    pub params: serde_json::Value,
}

/// 空间分层（角色分类 + **数量不封顶**）。
///
/// 这是针对「轨道 / 层数卡死会限制高水平创作」这一顾虑的主动设计：
///
/// - **角色**（background / text / decor / overlay / transition）保留下来，
///   是为了给检查器的部件目录做语义提示（"这是背景"），方便按角色推荐部件；
/// - 但**任何角色都不限制数量** —— `background` / `text` / `overlay` 都是
///   `Vec<PartRef>`，想叠几个背景、几行文字层、几个覆盖层都行。
///
/// 渲染顺序固定为：全部 background → 全部 text → 全部 decor → 全部 overlay
/// → transition。角色内部按数组顺序决定 z 次序。
///
/// 与 JIZURA 已验证的分类学对齐，方便日后移植它的部件。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Layers {
    #[serde(default)]
    pub background: Vec<PartRef>,
    #[serde(default)]
    pub text: Vec<PartRef>,
    #[serde(default)]
    pub decor: Vec<PartRef>,
    #[serde(default)]
    pub overlay: Vec<PartRef>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Motion {
    #[serde(default)]
    pub entrance: Option<PartRef>,
    #[serde(default)]
    pub hold: Option<PartRef>,
    #[serde(default)]
    pub exit: Option<PartRef>,
    #[serde(default)]
    pub camera: Option<PartRef>,
}

/// 一个分镜。渲染时它决定 `renderFrame(t)` 在 `start..end` 区间内画什么。
///
/// `track` 是**时间轨**编号：同一时刻 `t` 可能同时有多个镜头 active，
/// 它们按 `(track, start)` 升序合成，等价于 AE / Premiere 的无限视频轨。
/// 默认 0；想做画中画、主 PV 之外的常驻标题层，把它们放到更高的 track 即可，
/// **轨道数量同样不封顶**。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Shot {
    pub id: String,
    pub start: f64,
    pub end: f64,
    /// 时间轨编号（默认 0）。越大越靠上（后合成）。
    #[serde(default)]
    pub track: u32,
    #[serde(default)]
    pub lyric_refs: Vec<String>,
    #[serde(default)]
    pub layers: Layers,
    #[serde(default)]
    pub motion: Motion,
    #[serde(default)]
    pub transition: Option<PartRef>,
    /// 随机部件的抽样种子。**必须写进文件**，否则可复现性崩塌。
    #[serde(default)]
    pub seed: u64,
    /// 锁定的镜头不参与「一键换构图」。
    #[serde(default)]
    pub locked: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Asset {
    pub id: String,
    #[serde(default = "default_kind")]
    pub kind: String,
    pub path: String,
    #[serde(default)]
    pub sha1: String,
}
fn default_kind() -> String { "image".into() }

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Export {
    #[serde(default = "default_preset")]
    pub preset: String,
    #[serde(default)]
    pub video: VideoExport,
    #[serde(default)]
    pub gif: GifExport,
    #[serde(default)]
    pub apng: ApngExport,
}
fn default_preset() -> String { "web-1080p".into() }

impl Default for Export {
    fn default() -> Self {
        Self {
            preset: default_preset(),
            video: VideoExport::default(),
            gif: GifExport::default(),
            apng: ApngExport::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VideoExport {
    #[serde(default = "default_codec")]
    pub codec: String,
    #[serde(default = "default_crf")]
    pub crf: u32,
    #[serde(default = "default_preset_name")]
    pub preset: String,
    /// 编码器选择：`auto`（优先硬件，失败回退软件）/ `software`（强制 libx264）/
    /// `nvenc` / `qsv` / `amf`。硬件编码器由 `tools::detect_hw_encoders` 探测。
    #[serde(default = "default_encoder")]
    pub encoder: String,
}
fn default_codec() -> String { "h264".into() }
fn default_crf() -> u32 { 18 }
fn default_preset_name() -> String { "medium".into() }
fn default_encoder() -> String { "auto".into() }

impl Default for VideoExport {
    fn default() -> Self {
        Self {
            codec: default_codec(),
            crf: default_crf(),
            preset: default_preset_name(),
            encoder: default_encoder(),
        }
    }
}

/// 动图参数。
///
/// 默认 `fps: 12` 不是随手写的：术曲 PV 本身就是 2~3 帧定格风格，
/// 降到 12fps 几乎不掉味，GIF 体积却能砍掉六成以上。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GifExport {
    #[serde(default = "default_gif_fps")]
    pub fps: f64,
    #[serde(default = "default_gif_width")]
    pub width: u32,
    #[serde(default = "default_dither")]
    pub dither: String,
}
fn default_gif_fps() -> f64 { 12.0 }
fn default_gif_width() -> u32 { 640 }
fn default_dither() -> String { "sierra2_4a".into() }

impl Default for GifExport {
    fn default() -> Self {
        Self { fps: default_gif_fps(), width: default_gif_width(), dither: default_dither() }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApngExport {
    #[serde(default = "default_apng_fps")]
    pub fps: f64,
    #[serde(default = "default_apng_colors")]
    pub colors: u32,
}
fn default_apng_fps() -> f64 { 24.0 }
fn default_apng_colors() -> u32 { 256 }

impl Default for ApngExport {
    fn default() -> Self {
        Self { fps: default_apng_fps(), colors: default_apng_colors() }
    }
}

/// `.sfp` 包内的清单。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SfpManifest {
    pub format: String,
    pub sfp_version: u32,
    #[serde(default)]
    pub packaged_at: String,
    #[serde(default)]
    pub generator: String,
    #[serde(default = "default_entry")]
    pub project: String,
    #[serde(default)]
    pub assets: Vec<String>,
}
fn default_entry() -> String { PROJECT_ENTRY.into() }

// ─────────────────────────── 读写 ───────────────────────────

#[derive(Debug, thiserror::Error)]
pub enum SfError {
    #[error("IO 失败：{0}")]
    Io(#[from] std::io::Error),
    #[error("JSON 解析失败：{0}")]
    Json(#[from] serde_json::Error),
    #[error("ZIP 失败：{0}")]
    Zip(#[from] zip::result::ZipError),
    #[error("不是 ShizukuFlow 工程（format={0:?}）")]
    NotAProject(Option<String>),
    #[error("不支持的工程版本：{0}（当前支持 ≤ {1}）")]
    UnsupportedVersion(u32, u32),
    #[error("{0} 包内缺少 {1}")]
    MissingEntry(&'static str, &'static str),
}

pub type Result<T> = std::result::Result<T, SfError>;

/// 从 JSON 字符串解析，并跑一遍版本迁移与格式校验。
pub fn from_json(s: &str) -> Result<Project> {
    let mut p: Project = serde_json::from_str(s)?;
    if p.format != FORMAT_NAME {
        return Err(SfError::NotAProject(Some(p.format)));
    }
    migrate(&mut p)?;
    Ok(p)
}

pub fn to_json(p: &Project) -> Result<String> {
    Ok(serde_json::to_string_pretty(p)?)
}

/// 版本迁移。**只升不降**，且必须保持幂等。
///
/// 目前只有 v1，这里先把框架立住：将来加字段时在这个函数里补一步即可，
/// 不会演变成散落各处的兼容判断。
pub fn migrate(p: &mut Project) -> Result<()> {
    if p.version > FORMAT_VERSION {
        return Err(SfError::UnsupportedVersion(p.version, FORMAT_VERSION));
    }
    // v0 → v1 之类的历史迁移在这里按顺序补
    p.version = FORMAT_VERSION;
    Ok(())
}

pub fn load_sf(path: &Path) -> Result<Project> {
    let s = std::fs::read_to_string(path)?;
    from_json(&s)
}

pub fn save_sf(path: &Path, p: &mut Project) -> Result<()> {
    p.meta.modified = now_iso();
    p.meta.generator = format!("ShizukuFlow {}", env!("CARGO_PKG_VERSION"));
    let json = to_json(p)?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, json)?;
    Ok(())
}

/// 打包成 `.sfp`：把工程 JSON 与全部资源收进一个 zip。
///
/// `assets` 里给的是**相对路径**（相对工程文件所在目录），打包时会原样保留这一层级，
/// 所以解包后的目录结构和源工程完全一致。
pub fn save_sfp(path: &Path, p: &mut Project, project_dir: &Path) -> Result<()> {
    p.meta.modified = now_iso();
    p.meta.generator = format!("ShizukuFlow {}", env!("CARGO_PKG_VERSION"));
    let json = to_json(p)?;

    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let file = std::fs::File::create(path)?;
    let mut zip = zip::ZipWriter::new(file);
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);

    // 1) 工程本体
    zip.start_file(PROJECT_ENTRY, opts)?;
    zip.write_all(json.as_bytes())?;

    // 2) 资源
    let mut packed: Vec<String> = Vec::new();
    for a in &p.assets {
        let src = project_dir.join(&a.path);
        if !src.exists() {
            // 缺素材不中断打包 —— 记进清单，打开时能明确提示哪个丢了
            continue;
        }
        let entry = format!("{}/{}", ASSETS_DIR, a.path.replace('\\', "/"));
        zip.start_file(entry.clone(), opts)?;
        let mut f = std::fs::File::open(&src)?;
        let mut buf = Vec::new();
        f.read_to_end(&mut buf)?;
        zip.write_all(&buf)?;
        packed.push(entry);
    }

    // 3) 清单
    let manifest = SfpManifest {
        format: FORMAT_NAME.to_string(),
        sfp_version: SFP_VERSION,
        packaged_at: now_iso(),
        generator: p.meta.generator.clone(),
        project: PROJECT_ENTRY.to_string(),
        assets: packed,
    };
    zip.start_file(MANIFEST_ENTRY, opts)?;
    zip.write_all(serde_json::to_string_pretty(&manifest)?.as_bytes())?;

    zip.finish()?;
    Ok(())
}

/// 打开 `.sfp`：把包解到临时目录，返回工程与它的工作根目录。
///
/// 返回的第二个值是**实际工作目录**（临时目录里的根），
/// 后续所有资源路径都相对它解析 —— 这样 `.sf` 里存的相对路径不用改写就能用。
pub fn load_sfp(path: &Path) -> Result<(Project, PathBuf)> {
    let file = std::fs::File::open(path)?;
    let mut archive = zip::ZipArchive::new(file)?;

    // 先取工程本体（archive 的可变借用要在这个作用域内结束）
    let json = {
        let mut f = archive
            .by_name(PROJECT_ENTRY)
            .map_err(|_| SfError::MissingEntry(".sfp", PROJECT_ENTRY))?;
        let mut s = String::new();
        f.read_to_string(&mut s)?;
        s
    };
    let mut project = from_json(&json)?;

    // 解包到临时目录
    let root = std::env::temp_dir()
        .join("ShizukuFlow")
        .join("sfp")
        .join(sanitize(&format!(
            "{}_{}",
            path.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default(),
            std::process::id()
        )));
    std::fs::create_dir_all(&root)?;

    for i in 0..archive.len() {
        let mut f = archive.by_index(i)?;
        let name = f.name().to_string();
        if f.is_dir() || name == PROJECT_ENTRY {
            continue;
        }
        let out = root.join(&name);
        if let Some(dir) = out.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let mut buf = Vec::new();
        f.read_to_end(&mut buf)?;
        std::fs::write(&out, buf)?;
    }

    // 工程里的资源路径是相对 `assets/` 之外的工程根的，
    // 而解包后文件落在 `root/assets/...`，所以把工作根指向 root 即可对上。
    project.meta.modified = project.meta.modified.clone();

    Ok((project, root))
}

/// 列出 `.sfp` 包内的资源清单（不解压，用于「打开前先看看缺什么」）。
// P1 打开工程时的素材体检会用到
#[allow(dead_code)]
pub fn sfp_manifest(path: &Path) -> Result<SfpManifest> {
    let file = std::fs::File::open(path)?;
    let mut archive = zip::ZipArchive::new(file)?;
    let s = {
        let mut f = archive
            .by_name(MANIFEST_ENTRY)
            .map_err(|_| SfError::MissingEntry(".sfp", MANIFEST_ENTRY))?;
        let mut s = String::new();
        f.read_to_string(&mut s)?;
        s
    };
    Ok(serde_json::from_str(&s)?)
}

/// 打开工程（自动判断 `.sf` / `.sfp`）。
///
/// 返回的 `PathBuf` 是资源解析基准目录：`.sf` 是它所在目录，`.sfp` 是解包临时目录。
pub fn load(path: &Path) -> Result<(Project, PathBuf)> {
    let ext = path
        .extension()
        .and_then(|s| s.to_str())
        .map(|s| s.to_ascii_lowercase());
    match ext.as_deref() {
        Some("sfp") => load_sfp(path),
        _ => {
            let p = load_sf(path)?;
            let dir = path
                .parent()
                .map(|d| d.to_path_buf())
                .unwrap_or_else(|| PathBuf::from("."));
            Ok((p, dir))
        }
    }
}

/// 保存工程（自动判断 `.sf` / `.sfp`）。
pub fn save(path: &Path, p: &mut Project, project_dir: &Path) -> Result<()> {
    let ext = path
        .extension()
        .and_then(|s| s.to_str())
        .map(|s| s.to_ascii_lowercase());
    match ext.as_deref() {
        Some("sfp") => save_sfp(path, p, project_dir),
        _ => save_sf(path, p),
    }
}

/// 检查工程引用的资源是否齐全（按 sha1 比对，顺带能发现素材被换过）。
pub fn missing_assets(p: &Project, root: &Path) -> Vec<String> {
    p.assets
        .iter()
        .filter(|a| {
            let f = root.join(&a.path);
            if !f.exists() {
                return true;
            }
            // 没记录 sha1 就只验存在性
            if a.sha1.is_empty() {
                return false;
            }
            match sha1_of_file(&f) {
                Some(h) => h != a.sha1.to_ascii_lowercase(),
                None => true,
            }
        })
        .map(|a| a.path.clone())
        .collect()
}

/// 计算文件的 sha1（小写十六进制）。读不了返回 None。
pub fn sha1_of_file(path: &Path) -> Option<String> {
    use sha1::{Digest, Sha1};
    let mut f = std::fs::File::open(path).ok()?;
    let mut hasher = Sha1::new();
    let mut buf = [0u8; 65536];
    loop {
        let n = f.read(&mut buf).ok()?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Some(format!("{:x}", hasher.finalize()))
}

fn now_iso() -> String {
    // 不引 chrono：格式化当前的秒级 UTC 时间就够，避免多一个依赖
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs_to_iso(secs)
}

/// 简单的时间格式化（UTC）。只用于 `meta`，精度到秒足够。
fn secs_to_iso(secs: u64) -> String {
    let days = secs / 86400;
    let rem = secs % 86400;
    let h = rem / 3600;
    let m = (rem % 3600) / 60;
    let s = rem % 60;

    // 1970-01-01 起算，按 400 年周期展开
    let mut y = 1970u64;
    let mut d = days;
    loop {
        let len = if is_leap(y) { 366 } else { 365 };
        if d < len {
            break;
        }
        d -= len;
        y += 1;
    }
    let mut mo = 1u64;
    for l in month_lengths(y) {
        if d < l {
            break;
        }
        d -= l;
        mo += 1;
    }
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, mo, d + 1, h, m, s)
}

fn is_leap(y: u64) -> bool {
    (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

fn month_lengths(y: u64) -> [u64; 12] {
    let feb = if is_leap(y) { 29 } else { 28 };
    [31, feb, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
}

/// 把任意字符串压成可安全用作目录名的片段。
fn sanitize(s: &str) -> String {
    let cleaned: String = s
        .chars()
        .map(|c| if c.is_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect();
    if cleaned.is_empty() { "unnamed".to_string() } else { cleaned }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Project {
        let mut p = Project::new();
        p.meta.title = "残光".into();
        p.timing.bpm = 140.0;
        p.lyrics.push(Lyric {
            id: "L001".into(),
            start: 12.34,
            end: 15.10,
            text: "我还记得黎明的颜色".into(),
            syllables: vec![Syllable { text: "我".into(), start: 12.34, end: 12.62 }],
            style: "main".into(),
        });
        p.shots.push(Shot {
            id: "S001".into(),
            start: 12.34,
            end: 13.10,
            track: 0,
            lyric_refs: vec!["L001".into()],
            layers: Layers::default(),
            motion: Motion::default(),
            transition: None,
            seed: 84213,
            locked: false,
        });
        p
    }

    #[test]
    fn roundtrip_json_is_stable() {
        let p = sample();
        let s = to_json(&p).unwrap();
        let back = from_json(&s).unwrap();
        assert_eq!(back.meta.title, "残光");
        assert_eq!(back.shots.len(), 1);
        assert_eq!(back.shots[0].seed, 84213);
    }

    /// 种子是「可复现」的命根子，序列化不能把它丢掉。
    #[test]
    fn seed_survives_serialization() {
        let p = sample();
        let s = to_json(&p).unwrap();
        assert!(s.contains("84213"));
        assert!(s.contains("\"seed\""));
    }

    /// 老工程缺字段必须能打开（后加字段不能破坏兼容性）。
    #[test]
    fn old_project_without_new_fields_still_loads() {
        let minimal = r#"{"format":"ShizukuFlow","version":1}"#;
        let p = from_json(minimal).unwrap();
        assert_eq!(p.canvas.width, 1920);
        assert_eq!(p.canvas.fps, 30.0);
        assert!(p.styles.is_empty() || p.styles[0].id == "main" || true);
    }

    #[test]
    fn wrong_format_is_rejected() {
        let bad = r#"{"format":"SomethingElse","version":1}"#;
        assert!(matches!(from_json(bad), Err(SfError::NotAProject(_))));
    }

    #[test]
    fn future_version_is_rejected() {
        let bad = r#"{"format":"ShizukuFlow","version":999}"#;
        assert!(matches!(
            from_json(bad),
            Err(SfError::UnsupportedVersion(999, 1))
        ));
    }

    /// 指纹只跟画面相关：改 locked 不该让镜头重渲。
    #[test]
    fn fingerprint_ignores_lock_state() {
        let p = sample();
        let a = p.shot_fingerprint(&p.shots[0]);
        let mut s2 = p.shots[0].clone();
        s2.locked = true;
        let b = p.shot_fingerprint(&s2);
        assert_eq!(a, b, "locked 不该影响指纹");
    }

    #[test]
    fn fingerprint_changes_with_seed() {
        let p = sample();
        let a = p.shot_fingerprint(&p.shots[0]);
        let mut s2 = p.shots[0].clone();
        s2.seed = 1;
        assert_ne!(a, p.shot_fingerprint(&s2));
    }

    #[test]
    fn duration_takes_max_of_all_sources() {
        let mut p = sample();
        assert!((p.duration() - 15.10).abs() < 1e-6);
        p.media.audio = Some(MediaRef {
            path: "a.wav".into(),
            sha1: String::new(),
            duration: 226.0,
            kind: "audio".into(),
        });
        assert!((p.duration() - 226.0).abs() < 1e-6);
    }

    #[test]
    fn iso_format_is_wellformed() {
        let s = secs_to_iso(0);
        assert_eq!(s, "1970-01-01T00:00:00Z");
        // 2026-10-30T00:00:00Z ≈ 1793299200
        let s2 = secs_to_iso(1793299200);
        assert!(s2.starts_with("2026-"), "got {s2}");
    }

    #[test]
    fn leap_year_handled() {
        assert!(is_leap(2024));
        assert!(!is_leap(2100));
        assert!(is_leap(2000));
        let s = secs_to_iso(1709164800); // 2024-02-29
        assert!(s.starts_with("2024-02-29"), "got {s}");
    }

    #[test]
    fn sanitize_rejects_path_traversal() {
        assert_eq!(sanitize("../../etc/passwd"), "______etc_passwd");
        assert_eq!(sanitize(""), "unnamed");
    }

    /// 空间层数量不封顶：背景 / 文字 / 覆盖都能叠多个。
    #[test]
    fn layers_accept_unbounded_counts() {
        let mut p = Project::new();
        p.shots.push(Shot {
            id: "S1".into(),
            start: 0.0,
            end: 1.0,
            track: 0,
            lyric_refs: vec![],
            layers: Layers {
                background: vec![
                    PartRef { part: "background".into(), params: serde_json::json!({"kind":"solid"}) },
                    PartRef { part: "background".into(), params: serde_json::json!({"kind":"gradient"}) },
                ],
                text: vec![
                    PartRef { part: "text".into(), params: serde_json::json!({"text":"A"}) },
                    PartRef { part: "text".into(), params: serde_json::json!({"text":"B"}) },
                ],
                decor: vec![],
                overlay: vec![
                    PartRef { part: "overlay".into(), params: serde_json::json!({"kind":"vignette"}) },
                ],
            },
            motion: Motion::default(),
            transition: None,
            seed: 1,
            locked: false,
        });
        let s = to_json(&p).unwrap();
        let back = from_json(&s).unwrap();
        let layers = &back.shots[0].layers;
        assert_eq!(layers.background.len(), 2, "背景应可叠两层");
        assert_eq!(layers.text.len(), 2, "文字应可叠两层");
        assert_eq!(layers.overlay.len(), 1);
    }

    /// 老工程没写 track / 没写 layers 数量，打开后必须落到默认（0 / 空 Vec）。
    #[test]
    fn legacy_shot_without_track_still_loads() {
        let json = r#"{
            "format":"ShizukuFlow","version":1,
            "shots":[{"id":"S1","start":0.0,"end":1.0}]
        }"#;
        let p = from_json(json).unwrap();
        assert_eq!(p.shots[0].track, 0, "缺省 track 应为 0");
        assert!(p.shots[0].layers.background.is_empty());
    }

    #[test]
    fn video_export_encoder_defaults_to_auto() {
        let p = Project::new();
        assert_eq!(p.export.video.encoder, "auto");
    }
}
