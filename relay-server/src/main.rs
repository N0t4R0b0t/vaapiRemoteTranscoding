use std::net::SocketAddr;
use std::process::Stdio;
use std::time::Instant;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::process::Command;

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    command: Mode,
}

#[derive(Subcommand)]
enum Mode {
    /// Phase 1 test harness: pull a fixed source, transcode, serve MPEG-TS to
    /// any client. No libva/driver involved -- validates the transcode +
    /// XvMC playback pipe end-to-end before wiring in the real driver.
    Pull(PullArgs),
    /// Phase 3 mode: the driver pushes its H.264 bitstream over the
    /// connection, this transcodes it live, and streams MPEG-2 back on the
    /// same connection.
    Push(PushArgs),
}

#[derive(Parser)]
struct PullArgs {
    /// H.264 source URL/path that ffmpeg can read (file, rtsp://, http://, ...)
    #[arg(long, env = "RELAY_SOURCE")]
    source: String,

    #[command(flatten)]
    common: CommonArgs,
}

#[derive(Parser)]
struct PushArgs {
    #[command(flatten)]
    common: CommonArgs,
}

#[derive(Parser)]
struct CommonArgs {
    /// Address to listen on, e.g. 0.0.0.0:9100
    #[arg(long, env = "RELAY_LISTEN", default_value = "0.0.0.0:9100")]
    listen: String,

    /// Output resolution passed to ffmpeg's -s, e.g. 640x480
    #[arg(long, env = "RELAY_RESOLUTION", default_value = "640x480")]
    resolution: String,

    /// MPEG-2 quality (ffmpeg -q:v, lower is better quality)
    #[arg(long, env = "RELAY_QUALITY", default_value_t = 4)]
    quality: u32,

    /// Hardware-accelerated *decode* of the incoming source, via an
    /// Nvidia GPU passed through into this container (see
    /// deploy/proxmox/create-lxc.sh's GPU_PASSTHROUGH option).
    /// Decode-only, not a fully hardware-accelerated pipeline: NVENC
    /// (Nvidia's hardware encoder) has never supported MPEG-2 output --
    /// there's little modern demand for hardware-encoding *to* MPEG-2 --
    /// so the encode side stays on ffmpeg's native software mpeg2video
    /// encoder regardless of this setting. Still meaningful: decode is
    /// the heavier half of this transcode for most real H.264 sources.
    /// "none" (default) preserves the original all-software behavior
    /// unconditionally -- this flag only matters if the host actually
    /// has an Nvidia GPU passed through; it does nothing useful (and
    /// ffmpeg will simply fail to init the hwaccel) otherwise.
    #[arg(long, env = "RELAY_HWACCEL", default_value = "none")]
    hwaccel: String,
}

impl CommonArgs {
    /// ffmpeg hwaccel flags to insert immediately before the
    /// corresponding `-i`, or empty for software decode (the default).
    fn hwaccel_args(&self) -> Vec<&str> {
        match self.hwaccel.as_str() {
            "none" => vec![],
            // Deliberately no `-hwaccel_output_format cuda` here: that
            // would keep decoded frames in GPU memory for a zero-copy
            // handoff to nvenc, which doesn't apply here (see this
            // struct's doc comment on why encode stays software) --
            // omitting it makes ffmpeg copy decoded frames back to
            // normal system memory automatically, exactly what the
            // software mpeg2video encoder needs to consume them.
            "nvdec" => vec!["-hwaccel", "cuda"],
            other => {
                tracing::warn!(hwaccel = other, "unrecognized RELAY_HWACCEL value, ignoring (using software decode)");
                vec![]
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();

    match Cli::parse().command {
        Mode::Pull(args) => run_pull(args).await,
        Mode::Push(args) => run_push(args).await,
    }
}

async fn run_pull(args: PullArgs) -> Result<()> {
    let listener = bind(&args.common.listen).await?;
    tracing::info!(listen = %args.common.listen, source = %args.source, mode = "pull", "relay-server up");

    loop {
        let (socket, peer) = listener.accept().await?;
        let source = args.source.clone();
        let resolution = args.common.resolution.clone();
        let quality = args.common.quality;
        let hwaccel_args: Vec<String> = args.common.hwaccel_args().into_iter().map(String::from).collect();

        tokio::spawn(async move {
            if let Err(e) = serve_pull_connection(socket, peer, &source, &resolution, quality, &hwaccel_args).await {
                tracing::warn!(%peer, error = %e, "pull connection ended with error");
            }
        });
    }
}

async fn run_push(args: PushArgs) -> Result<()> {
    let listener = bind(&args.common.listen).await?;
    tracing::info!(listen = %args.common.listen, mode = "push", "relay-server up");

    loop {
        let (socket, peer) = listener.accept().await?;
        let resolution = args.common.resolution.clone();
        let quality = args.common.quality;
        let hwaccel_args: Vec<String> = args.common.hwaccel_args().into_iter().map(String::from).collect();

        tokio::spawn(async move {
            if let Err(e) = serve_push_connection(socket, peer, &resolution, quality, &hwaccel_args).await {
                tracing::warn!(%peer, error = %e, "push connection ended with error");
            }
        });
    }
}

async fn bind(listen: &str) -> Result<TcpListener> {
    TcpListener::bind(listen)
        .await
        .with_context(|| format!("binding {listen}"))
}

async fn serve_pull_connection(
    mut socket: TcpStream,
    peer: SocketAddr,
    source: &str,
    resolution: &str,
    quality: u32,
    hwaccel_args: &[String],
) -> Result<()> {
    let accepted_at = Instant::now();
    tracing::info!(%peer, "client connected, starting transcode");

    let quality_str = quality.to_string();
    let mut args: Vec<&str> = vec!["-loglevel", "warning", "-re"];
    args.extend(hwaccel_args.iter().map(String::as_str));
    args.extend([
        "-i", source,
        "-c:v", "mpeg2video",
        "-q:v", &quality_str,
        "-s", resolution,
        "-f", "mpegts",
        "pipe:1",
    ]);

    let mut child = Command::new("ffmpeg")
        .args(&args)
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .kill_on_drop(true)
        .spawn()
        .context("spawning ffmpeg")?;

    let stdout = child.stdout.take().expect("ffmpeg stdout was piped");
    copy_logging_first_byte(stdout, &mut socket, peer, accepted_at).await?;

    let status = child.wait().await.context("waiting on ffmpeg")?;
    tracing::info!(%peer, ?status, "transcode finished");
    Ok(())
}

async fn serve_push_connection(
    socket: TcpStream,
    peer: SocketAddr,
    resolution: &str,
    quality: u32,
    hwaccel_args: &[String],
) -> Result<()> {
    let accepted_at = Instant::now();
    tracing::info!(%peer, "driver connected, starting live transcode");

    let quality_str = quality.to_string();
    let mut args: Vec<&str> = vec![
        "-loglevel", "warning",
        // The synchronous per-picture protocol (see the driver's
        // xvmc_relay_end_picture) sends one picture's worth of H.264
        // at a time, then blocks reading the corresponding MPEG-2
        // output -- so ffmpeg must start decoding from just an
        // SPS/PPS/first-slice, not wait for its default probing
        // (avformat_find_stream_info scans up to -probesize/
        // -analyzeduration worth of data before returning, which
        // deadlocks against the driver's blocking recv() once nothing
        // else arrives). -probesize 32 plus -analyzeduration 0 make
        // it commit to a format guess from the minimum possible data,
        // the standard trick for piping a live raw H.264 stream.
        "-probesize", "32",
        "-analyzeduration", "0",
        // The synthesized SPS this driver's client sends omits VUI
        // timing info (out of scope for a minimal SPS), so without a
        // framerate hint ffmpeg's demuxer tries to estimate one from
        // multiple frames' timestamps -- with only one picture in
        // flight at a time (see above), "not enough frames to
        // estimate rate" makes it stall for several seconds before
        // giving up and proceeding anyway. Telling it the input rate
        // directly skips that estimation entirely. 25 is a
        // placeholder default -- VA-API doesn't hand the driver a
        // real frame rate to forward here.
        "-r", "25",
        "-f", "h264",
    ];
    // Must come immediately before -i for this specific input, same as
    // pull mode -- see CommonArgs::hwaccel_args's comment for why this
    // is decode-only and never touches the encode side below.
    args.extend(hwaccel_args.iter().map(String::as_str));
    args.extend([
        "-i", "pipe:0",
        "-c:v", "mpeg2video",
        "-q:v", &quality_str,
        "-s", resolution,
        // Raw MPEG-2 elementary stream, not MPEG-TS: the driver-side
        // client parses MPEG-2 sequence/picture/slice headers directly
        // out of this stream (see vaapi-xvmc-driver/src/mpeg2_headers.[ch]),
        // so there's no reason to make it also demux a TS/PES container
        // first. Pull mode (above) is unaffected -- MPEG-TS there is
        // already tested end-to-end and consumed by things (mplayer,
        // ffprobe) that expect a container.
        "-f", "mpeg2video",
        "pipe:1",
    ]);

    let mut child = Command::new("ffmpeg")
        .args(&args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .kill_on_drop(true)
        .spawn()
        .context("spawning ffmpeg")?;

    let mut ffmpeg_stdin = child.stdin.take().expect("ffmpeg stdin was piped");
    let ffmpeg_stdout = child.stdout.take().expect("ffmpeg stdout was piped");
    let (mut sock_read, mut sock_write) = socket.into_split();

    // Client -> ffmpeg (the incoming H.264 bitstream). Ends when the driver
    // closes its write side (end of playback) or the connection drops;
    // dropping ffmpeg_stdin afterward tells ffmpeg it's seen EOF.
    let upload = tokio::spawn(async move {
        let result = tokio::io::copy(&mut sock_read, &mut ffmpeg_stdin)
            .await
            .context("copying client bitstream to ffmpeg stdin");
        drop(ffmpeg_stdin);
        result
    });

    // ffmpeg -> client (the transcoded MPEG-2 stream going back).
    let download = copy_logging_first_byte(ffmpeg_stdout, &mut sock_write, peer, accepted_at);

    let (upload_result, download_result) = tokio::join!(upload, download);
    upload_result.context("upload task panicked")??;
    download_result?;

    let status = child.wait().await.context("waiting on ffmpeg")?;
    tracing::info!(%peer, ?status, "live transcode finished");
    Ok(())
}

async fn copy_logging_first_byte<R, W>(
    mut reader: R,
    writer: &mut W,
    peer: SocketAddr,
    accepted_at: Instant,
) -> Result<()>
where
    R: AsyncRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let mut buf = [0u8; 64 * 1024];
    let mut first_byte_logged = false;

    loop {
        let n = reader.read(&mut buf).await.context("reading transcoded output")?;
        if n == 0 {
            break;
        }
        if !first_byte_logged {
            tracing::info!(%peer, cold_start_ms = accepted_at.elapsed().as_millis(), "first byte to client");
            first_byte_logged = true;
        }
        writer.write_all(&buf[..n]).await.context("writing to client")?;
    }
    Ok(())
}
