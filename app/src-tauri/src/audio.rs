//! 音频解码 + 波形峰值金字塔（多声道 / 声场视图）。
//!
//! # 为什么需要金字塔
//!
//! Aegisub 那种「缩到整首歌还能流畅刮擦波形」的手感，靠的是**多级峰值缓存**：
//! 预先把 PCM 按 256 / 1024 / 4096 … 样本聚合成 (min,max) 峰值桶。
//! 绘制某一段时，按「每像素样本数」挑一个粒度合适的层级，
//! 每个像素列只需聚合少数几个桶 —— 无论缩放到什么比例，
//! 耗时都只跟**画布宽度**成正比，而不是跟歌曲长度成正比。
//!
//! 这是 P0 最不能偷懒的一处：后期再补要重写整套绘制逻辑，
//! 而且直接决定「像不像 Aegisub」这个核心目标。
//!
//! # 声道策略（为什么不再强制单声道）
//!
//! 早期版本把音轨下混成单声道（照抄 Aegisub 显示习惯），但有两个隐患：
//! 1. 丢掉声场信息 —— 术曲里的混响、声像摆动、人声居中/伴奏拉宽全在左右差里；
//! 2. 若渲染母版时拿解码出的单声道 PCM 二次编码，原曲立体声永久丢失。
//!
//! 现在的做法：
//! - 解码**保留原始声道数**（不强制单声道）；
//! - 波形默认以 **Mix（下混）** 显示，维持 Aegisub「总能量」手感；
//! - 另提供 **Left / Right / Mid / Side** 三种声场视图，术曲制作里常用
//!   Mid/Side 区分人声（居中）与伴奏（拉宽）；
//! - 导出时**永远直接 mux 原始音轨**，绝不用这里解码出的 PCM 二次编码，
//!   立体声信息不会因预览而下采样丢失。
//!
//! # 数据流
//!
//! 音轨文件 ──(ffprobe)──> 声道数 / 时长
//!          ──(ffmpeg)──> 原始声道数 44.1k s16 PCM(交织)
//!                          └──> 去交织为平面存储(每声道一份)
//!                                  └──> 每声道独立金字塔(内存)
//!                                          └──> 按视图聚合 ──> 归一化峰值

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use serde::{Deserialize, Serialize};

/// 固定解码参数：44.1k、16bit。声道数保留原始。
const SAMPLE_RATE: u32 = 44_100;
const SAMPLE_FMT: &str = "s16"; // 16-bit 有符号整型
const BASE_BUCKET: usize = 256; // 最细一级：256 样本/桶 ≈ 5.8ms
/// 金字塔层级：每级桶大小 = BASE_BUCKET * 4^k，直到覆盖不了全曲。
const LEVEL_FACTOR: usize = 4;

/// 单声道的一段区间峰值。
#[derive(Debug, Clone)]
struct Level {
    bucket: usize,
    mins: Vec<i16>,
    maxs: Vec<i16>,
}

/// 单声道峰值数据（含原始 PCM，供极致放大时回退到逐样本）。
struct ChannelData {
    pcm: Vec<i16>,
    levels: Vec<Level>,
}

/// 整首歌的峰值金字塔：每个声道一份独立数据。
pub struct Pyramid {
    pub sample_rate: u32,
    pub total_samples: usize, // 每声道样本数
    pub num_channels: u32,
    channels: Vec<ChannelData>,
}

/// 波形声场视图。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ChannelView {
    /// 下混（所有声道平均）—— 默认，Aegisub 同款手感
    Mix,
    /// 第 0 声道
    Left,
    /// 第 1 声道（多声道时退化为最后一轨）
    Right,
    /// 中置 (L+R)/2 —— 人声通常集中在这里
    Mid,
    /// 侧边 (L-R)/2 —— 空间感 / 伴奏宽度
    Side,
}

/// 给前端的波形数据：每段区间返回交错的 [min0,max0,min1,max1,...]，
/// 值已归一化到 [-1,1]。
#[derive(Debug, Clone, Serialize)]
pub struct Waveform {
    pub t0: f64,
    pub t1: f64,
    pub width: u32,
    /// 长度 = width*2，偶数位是 min、奇数位是 max。
    pub peaks: Vec<f32>,
}

/// 音轨元信息（来自 ffprobe / 解码结果）。
#[derive(Debug, Clone, Serialize)]
pub struct AudioMeta {
    pub duration: f64,
    pub sample_rate: u32,
    pub channels: u32,
    pub has_audio: bool,
}

impl Pyramid {
    /// 由平面 PCM（每声道一份）构建金字塔。
    fn build(planar: Vec<Vec<i16>>, sample_rate: u32) -> Pyramid {
        let num_channels = planar.len() as u32;
        let total = planar.first().map(|c| c.len()).unwrap_or(0);
        let channels = planar
            .into_iter()
            .map(|pcm| {
                let mut levels: Vec<Level> = Vec::new();
                let mut bucket = BASE_BUCKET;
                while bucket < total {
                    let n = total / bucket;
                    let mut mins = Vec::with_capacity(n);
                    let mut maxs = Vec::with_capacity(n);
                    for i in 0..n {
                        let s = i * bucket;
                        let e = ((i + 1) * bucket).min(total);
                        let mut mn = i16::MAX;
                        let mut mx = i16::MIN;
                        for &v in &pcm[s..e] {
                            if v < mn {
                                mn = v;
                            }
                            if v > mx {
                                mx = v;
                            }
                        }
                        mins.push(mn);
                        maxs.push(mx);
                    }
                    levels.push(Level { bucket, mins, maxs });
                    if bucket > total / LEVEL_FACTOR {
                        break;
                    }
                    bucket *= LEVEL_FACTOR;
                }
                ChannelData { pcm, levels }
            })
            .collect();
        Pyramid {
            sample_rate,
            total_samples: total,
            num_channels,
            channels,
        }
    }

    /// 取某帧在指定视图下的合成样本值（i32 避免 Mid/Side 中间计算溢出）。
    fn view_sample(&self, view: ChannelView, f: usize) -> i32 {
        let nch = self.channels.len();
        match view {
            ChannelView::Mix => {
                if nch == 0 {
                    return 0;
                }
                let sum: i32 = self.channels.iter().map(|c| c.pcm[f] as i32).sum();
                sum.div_euclid(nch as i32)
            }
            ChannelView::Left => self.channels[0].pcm[f] as i32,
            ChannelView::Right => self.channels[nch.min(2) - 1].pcm[f] as i32,
            ChannelView::Mid => {
                if nch < 2 {
                    return self.channels[0].pcm[f] as i32;
                }
                ((self.channels[0].pcm[f] as i32) + (self.channels[1].pcm[f] as i32)) / 2
            }
            ChannelView::Side => {
                if nch < 2 {
                    return 0;
                }
                ((self.channels[0].pcm[f] as i32) - (self.channels[1].pcm[f] as i32)) / 2
            }
        }
    }

    /// 取某层级在指定视图下的包络 (mins, maxs)。
    /// 单声道视图直接复用该声道层级；Mix/Mid/Side 由声道 0/1 的桶数据派生。
    fn view_level(&self, view: ChannelView, lvl_idx: usize) -> (Vec<i16>, Vec<i16>) {
        let nch = self.channels.len();
        match view {
            ChannelView::Left => {
                let l = &self.channels[0].levels[lvl_idx];
                (l.mins.clone(), l.maxs.clone())
            }
            ChannelView::Right => {
                let l = &self.channels[nch.min(2) - 1].levels[lvl_idx];
                (l.mins.clone(), l.maxs.clone())
            }
            ChannelView::Mix => {
                let mut mins = self.channels[0].levels[lvl_idx].mins.clone();
                let mut maxs = self.channels[0].levels[lvl_idx].maxs.clone();
                for ch in &self.channels[1..] {
                    let l = &ch.levels[lvl_idx];
                    for i in 0..mins.len() {
                        if l.mins[i] < mins[i] {
                            mins[i] = l.mins[i];
                        }
                        if l.maxs[i] > maxs[i] {
                            maxs[i] = l.maxs[i];
                        }
                    }
                }
                (mins, maxs)
            }
            ChannelView::Mid => {
                if nch < 2 {
                    let l = &self.channels[0].levels[lvl_idx];
                    return (l.mins.clone(), l.maxs.clone());
                }
                let a = &self.channels[0].levels[lvl_idx];
                let b = &self.channels[1].levels[lvl_idx];
                let mut mins = Vec::with_capacity(a.mins.len());
                let mut maxs = Vec::with_capacity(a.maxs.len());
                for i in 0..a.mins.len() {
                    mins.push(((a.mins[i] as i32 + b.mins[i] as i32) / 2) as i16);
                    maxs.push(((a.maxs[i] as i32 + b.maxs[i] as i32) / 2) as i16);
                }
                (mins, maxs)
            }
            ChannelView::Side => {
                if nch < 2 {
                    let n = self.channels[0].levels[lvl_idx].mins.len();
                    return (vec![0i16; n], vec![0i16; n]);
                }
                let a = &self.channels[0].levels[lvl_idx];
                let b = &self.channels[1].levels[lvl_idx];
                let mut mins = Vec::with_capacity(a.mins.len());
                let mut maxs = Vec::with_capacity(a.maxs.len());
                for i in 0..a.mins.len() {
                    // side = (L - R)/2 的安全包络
                    mins.push(((a.mins[i] as i32 - b.maxs[i] as i32) / 2) as i16);
                    maxs.push(((a.maxs[i] as i32 - b.mins[i] as i32) / 2) as i16);
                }
                (mins, maxs)
            }
        }
    }

    /// 把 [t0, t1] 区间聚合为 `width` 列峰值（按 `view` 声场视图）。
    ///
    /// 性能关键：每像素列只聚合常数个桶（或放大时常数个样本），
    /// 整体 O(width)，与歌曲长度无关。
    pub fn peaks(&self, t0: f64, t1: f64, width: u32, view: ChannelView) -> Waveform {
        let width = width.max(1) as usize;
        let sr = self.sample_rate as f64;
        let start = (t0 * sr).max(0.0) as usize;
        let end = (t1 * sr).min(self.total_samples as f64) as usize;
        let span = (end - start).max(1);
        let s_per_px = span as f64 / width as f64;
        let nch = self.channels.len();
        let mut peaks: Vec<f32> = Vec::with_capacity(width * 2);

        if s_per_px <= BASE_BUCKET as f64 || nch == 0 {
            // 放大态（或短到没建出任何层级）：逐样本 min/max 回退。
            // 此时每像素样本数很少，开销可忽略；Mix/Mid/Side 在这里逐帧合成。
            for x in 0..width {
                let a = start + (x as f64 * s_per_px) as usize;
                let mut b = start + ((x + 1) as f64 * s_per_px) as usize;
                b = b.min(end).max(a + 1);
                let a = a.min(self.total_samples);
                let b = b.min(self.total_samples);
                if b <= a {
                    peaks.push(0.0);
                    peaks.push(0.0);
                    continue;
                }
                let mut mn = i32::MAX;
                let mut mx = i32::MIN;
                for f in a..b {
                    let v = self.view_sample(view, f);
                    if v < mn {
                        mn = v;
                    }
                    if v > mx {
                        mx = v;
                    }
                }
                peaks.push(mn as f32 / 32768.0);
                peaks.push(mx as f32 / 32768.0);
            }
        } else {
            // 缩略态：挑粒度最细、且桶大小 ≤ 每像素样本数的层级（各声道层级计划一致）
            let first = &self.channels[0].levels;
            let mut lvl_idx = 0;
            for (i, l) in first.iter().enumerate() {
                if (l.bucket as f64) <= s_per_px {
                    lvl_idx = i;
                } else {
                    break;
                }
            }
            let (vmins, vmaxs) = self.view_level(view, lvl_idx);
            let b = first[lvl_idx].bucket;
            for x in 0..width {
                let a = start + (x as f64 * s_per_px) as usize;
                let be = start + ((x + 1) as f64 * s_per_px) as usize;
                let i0 = a / b;
                let mut i1 = (be / b).max(i0 + 1);
                i1 = i1.min(vmins.len());
                let mut mn = i16::MAX;
                let mut mx = i16::MIN;
                for i in i0..i1 {
                    if vmins[i] < mn {
                        mn = vmins[i];
                    }
                    if vmaxs[i] > mx {
                        mx = vmaxs[i];
                    }
                }
                peaks.push(mn as f32 / 32768.0);
                peaks.push(mx as f32 / 32768.0);
            }
        }

        Waveform {
            t0,
            t1,
            width: width as u32,
            peaks,
        }
    }
}

/// 进程内缓存：键是音轨绝对路径（稳定且唯一）。
///
/// 存 `Arc<Pyramid>` 而非 `Pyramid` —— 金字塔含整首 PCM（立体声约 42MB），
/// 若每次取波形都 clone 整份数据，刮擦时每帧都要搬几十 MB，直接卡死。
/// 用 Arc 后取用只是 +1 引用计数，零拷贝。
pub struct AudioCache {
    map: Mutex<HashMap<String, Arc<Pyramid>>>,
}

impl AudioCache {
    pub fn new() -> Self {
        AudioCache {
            map: Mutex::new(HashMap::new()),
        }
    }

    /// 取已加载的金字塔（Arc，零拷贝）；没有返回 None（调用方决定要不要先 load）。
    pub fn get(&self, key: &str) -> Option<Arc<Pyramid>> {
        self.map.lock().unwrap().get(key).cloned()
    }

    /// 放入缓存。
    pub fn insert(&self, key: String, p: Pyramid) {
        self.map.lock().unwrap().insert(key, Arc::new(p));
    }
}

/// 缓存键：音轨路径的哈希。同一文件多次加载复用 PCM 磁盘缓存。
fn cache_key(input: &str) -> String {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    input.hash(&mut h);
    format!("{:016x}", h.finish())
}

/// 源文件指纹（mtime + 大小），用于判断 PCM 缓存是否过期。
fn source_stamp(input: &Path) -> Option<(u64, u64)> {
    let m = std::fs::metadata(input).ok()?;
    let mtime = m
        .modified()
        .ok()?
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    Some((mtime, m.len()))
}

fn stamp_path(cache_dir: &Path, key: &str) -> PathBuf {
    cache_dir.join(format!("{key}.stamp"))
}
fn pcm_path(cache_dir: &Path, key: &str) -> PathBuf {
    cache_dir.join(format!("{key}.pcm"))
}

/// ffprobe 取声道数与时长（失败回退 2 声道、时长 0）。
fn probe(input: &str, tools_dir: &str) -> (u32, f64) {
    let fp = crate::tools::tool(tools_dir, &crate::tools::bin_name("ffprobe"));
    let out = std::process::Command::new(&fp)
        .args([
            "-v",
            "error",
            "-show_entries",
            "stream=channels:format=duration",
            "-of",
            "json",
            "-i",
            input,
        ])
        .output();
    if let Ok(o) = out {
        if o.status.success() {
            if let Ok(v) = serde_json::from_slice::<serde_json::Value>(&o.stdout) {
                let ch = v["streams"]
                    .as_array()
                    .and_then(|a| a.iter().find(|x| x["channels"].is_i64()))
                    .and_then(|x| x["channels"].as_i64())
                    .unwrap_or(2) as u32;
                let dur = v["format"]["duration"]
                    .as_str()
                    .and_then(|d| d.parse::<f64>().ok())
                    .unwrap_or(0.0);
                return (ch.max(1), dur);
            }
        }
    }
    (2, 0.0)
}

/// 用自备 ffmpeg 把音轨解码为「原始声道数」44.1k s16 PCM（交织），写到 pcm_path。
fn decode_to_pcm(
    input: &str,
    pcm_path: &Path,
    tools_dir: &str,
    channels: u32,
) -> Result<(), String> {
    let ff = crate::tools::tool(tools_dir, &crate::tools::bin_name("ffmpeg"));
    let mut cmd = std::process::Command::new(&ff);
    cmd.args([
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        input,
        "-vn", // 不要视频
        "-ac",
        &channels.to_string(), // 保留原始声道数（不强制单声道）
        "-ar",
        &SAMPLE_RATE.to_string(),
        "-sample_fmt",
        SAMPLE_FMT,
        "-f",
        "s16le",
        "-map",
        "0:a:0", // 第一条音轨；没有音轨时 ffmpeg 会报错，下面捕获
        "-y",
        &pcm_path.to_string_lossy(),
    ]);
    crate::tools::no_window(&mut cmd);

    let out = cmd
        .output()
        .map_err(|e| format!("无法启动 ffmpeg（{ff}）：{e}。请检查工具链是否就绪。"))?;
    if !out.status.success() {
        let msg = String::from_utf8_lossy(&out.stderr);
        return Err(format!(
            "ffmpeg 解码失败：{msg}\n（源文件可能不含音轨或格式不受支持）"
        ));
    }
    Ok(())
}

/// 读取交织 PCM 并去交织为平面存储（每声道一份）。
fn read_pcm_planar(pcm_path: &Path, channels: u32) -> Result<Vec<Vec<i16>>, String> {
    let bytes = std::fs::read(pcm_path).map_err(|e| format!("读取 PCM 缓存失败：{e}"))?;
    let nch = channels as usize;
    if nch == 0 || bytes.len() % (2 * nch) != 0 {
        return Err("PCM 缓存长度异常（声道数或 16bit 对齐不符）".into());
    }
    let frames = bytes.len() / (2 * nch);
    let mut out: Vec<Vec<i16>> = (0..nch).map(|_| Vec::with_capacity(frames)).collect();
    for f in 0..frames {
        for c in 0..nch {
            let o = (f * nch + c) * 2;
            out[c].push(i16::from_le_bytes([bytes[o], bytes[o + 1]]));
        }
    }
    Ok(out)
}

/// `load_with_pyramid` 的返回：同时给出元信息与可直接缓存的金字塔。
pub struct LoadedAudio {
    pub meta: AudioMeta,
    pub pyramid: Pyramid,
}

/// 加载音轨：优先复用磁盘 PCM 缓存，否则用 ffmpeg 解码并落盘，再构建金字塔。
///
/// `cache_dir` 建议用工程目录下的缓存子目录（如 `<project>/.cache/`）。
/// 返回元信息与金字塔，由 cmd 层负责塞进进程内 `AudioCache`。
pub fn load_with_pyramid(
    input: &str,
    cache_dir: &str,
    tools_dir: &str,
) -> Result<LoadedAudio, String> {
    let in_path = Path::new(input);
    let cache = PathBuf::from(cache_dir);
    std::fs::create_dir_all(&cache).map_err(|e| format!("创建缓存目录失败：{e}"))?;

    let key = cache_key(input);
    let pcm_file = pcm_path(&cache, &key);
    let stamp_file = stamp_path(&cache, &key);
    let stamp = source_stamp(in_path);

    // 声道数由音轨本身决定（保留原始声道数，不强制单声道）
    let (channels, _dur_probe) = probe(input, tools_dir);

    let reuse = match (&stamp, std::fs::read_to_string(&stamp_file).ok()) {
        (Some((m, s)), Some(saved)) if saved.trim() == format!("{m}:{s}") => pcm_file.exists(),
        _ => false,
    };

    let planar = if reuse {
        read_pcm_planar(&pcm_file, channels)?
    } else {
        decode_to_pcm(input, &pcm_file, tools_dir, channels)?;
        let planar = read_pcm_planar(&pcm_file, channels)?;
        if let Some((m, s)) = stamp {
            let _ = std::fs::write(&stamp_file, format!("{m}:{s}"));
        }
        planar
    };

    let total = planar.first().map(|c| c.len()).unwrap_or(0);
    let pyramid = Pyramid::build(planar, SAMPLE_RATE);
    let duration = total as f64 / SAMPLE_RATE as f64;
    Ok(LoadedAudio {
        meta: AudioMeta {
            duration,
            sample_rate: SAMPLE_RATE,
            channels,
            has_audio: total > 0,
        },
        pyramid,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一段合成 PCM（含一个明显的「鼓点」脉冲），验证金字塔能抓住极值。
    fn synthetic_mono(n: usize) -> Vec<Vec<i16>> {
        let mut v = vec![0i16; n];
        for i in 0..n {
            let phase = i % 1000;
            let pulse = if phase < 20 { 30000i16 } else { 0 };
            let noise = ((i as f64 * 0.123).sin() * 800.0) as i16;
            v[i] = pulse + noise;
        }
        vec![v]
    }

    #[test]
    fn pyramid_captures_peak() {
        let pyr = Pyramid::build(synthetic_mono(200_000), SAMPLE_RATE);
        assert!(!pyr.channels[0].levels.is_empty(), "至少应有一个层级");
        let w = pyr.peaks(0.0, 0.05, 100, ChannelView::Mix);
        assert_eq!(w.peaks.len(), 200);
        let max_val = w
            .peaks
            .iter()
            .skip(1)
            .step_by(2)
            .cloned()
            .fold(-1.0f32, f32::max);
        assert!(max_val > 0.8, "峰值应接近 1.0（30000/32768≈0.915），实际 {max_val}");
    }

    #[test]
    fn peaks_length_matches_width() {
        let pyr = Pyramid::build(synthetic_mono(1_000_000), SAMPLE_RATE);
        let w = pyr.peaks(0.0, pyr.total_samples as f64 / SAMPLE_RATE as f64, 800, ChannelView::Mix);
        assert_eq!(w.peaks.len(), 1600);
    }

    #[test]
    fn zoomed_in_falls_back_to_samples() {
        let pyr = Pyramid::build(synthetic_mono(500_000), SAMPLE_RATE);
        let w = pyr.peaks(1.0, 1.01, 200, ChannelView::Mix);
        assert_eq!(w.peaks.len(), 400);
    }

    #[test]
    fn empty_pcm_safe() {
        let pyr = Pyramid::build(vec![], SAMPLE_RATE);
        let w = pyr.peaks(0.0, 1.0, 10, ChannelView::Mix);
        assert_eq!(w.peaks.len(), 20);
        assert!(w.peaks.iter().all(|&x| x == 0.0));
    }

    /// 双声道：Mid 视图应等于 (L+R)/2，Side 视图应等于 (L-R)/2。
    #[test]
    fn stereo_mid_side_views() {
        // L 全 +10000，R 全 -10000 → Mid 应≈0，Side 应≈+10000
        let n = 10_000usize;
        let l = vec![10000i16; n];
        let r = vec![-10000i16; n];
        let pyr = Pyramid::build(vec![l, r], SAMPLE_RATE);
        assert_eq!(pyr.num_channels, 2);

        // 放大态（逐样本）：取一段看 Mid/Side 合成值
        let w_mid = pyr.peaks(0.0, 0.05, 50, ChannelView::Mid);
        let w_side = pyr.peaks(0.0, 0.05, 50, ChannelView::Side);
        let mid_max = w_mid.peaks.iter().skip(1).step_by(2).cloned().fold(-1.0f32, f32::max);
        let side_max = w_side
            .peaks
            .iter()
            .skip(1)
            .step_by(2)
            .cloned()
            .fold(-1.0f32, f32::max);
        // Mid ≈ 0（±噪声容差），Side ≈ 10000/32768 ≈ 0.305
        assert!(mid_max.abs() < 0.02, "Mid 应接近 0，实际 {mid_max}");
        assert!(side_max > 0.28 && side_max < 0.33, "Side 应≈0.305，实际 {side_max}");
    }

    #[test]
    fn stereo_left_right_distinct() {
        let n = 10_000usize;
        let l = vec![20000i16; n];
        let r = vec![0i16; n];
        let pyr = Pyramid::build(vec![l, r], SAMPLE_RATE);
        // 缩略态下，Left 应抓到 ~0.61，Right 应≈0
        let w_l = pyr.peaks(0.0, n as f64 / SAMPLE_RATE as f64, 100, ChannelView::Left);
        let w_r = pyr.peaks(0.0, n as f64 / SAMPLE_RATE as f64, 100, ChannelView::Right);
        let l_max = w_l.peaks.iter().skip(1).step_by(2).cloned().fold(-1.0f32, f32::max);
        let r_max = w_r.peaks.iter().skip(1).step_by(2).cloned().fold(-1.0f32, f32::max);
        assert!(l_max > 0.58, "Left 应接近 0.61，实际 {l_max}");
        assert!(r_max < 0.01, "Right 应≈0，实际 {r_max}");
    }
}
