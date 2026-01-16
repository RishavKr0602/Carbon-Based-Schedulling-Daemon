use anyhow::{Context, Result};
use clap::Parser;
use reqwest::Client;
use serde::Deserialize;
use std::fs::{self, File};
use std::io::Write;
use std::process::Stdio;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::process::Command;
use tokio::sync::RwLock;
use tokio::time::sleep;
use tracing::{error, info, warn};
use tracing_appender::rolling;
use tracing_subscriber::{fmt, EnvFilter, prelude::*};

const DEFAULT_CONFIG: &str = "/mnt/storage/osproject/tasks.json";
const DEFAULT_LOG: &str = "/tmp/green_scheduler.log";
const DEFAULT_PID: &str = "/var/run/green_scheduler.pid";
const CARBON_API_URL: &str = "https://api.carbonintensity.org.uk/intensity";

#[derive(Parser, Debug)]
#[command(author, version, about = "Carbon-aware task scheduler (Rust)")]
struct Args {
    #[arg(short = 'f', long = "foreground")]
    foreground: bool,

    #[arg(short = 'c', long = "config", default_value = DEFAULT_CONFIG)]
    config: String,

    #[arg(short = 'l', long = "log", default_value = DEFAULT_LOG)]
    log: String,

    #[arg(short = 'p', long = "pid", default_value = DEFAULT_PID)]
    pid: String,

    #[arg(short = 'i', long = "interval", default_value_t = 300)]
    interval: u64,
}

#[derive(Debug, Deserialize, Clone)]
struct TaskConfig {
    command: String,
    urgency: String,
    deadline_hours: Option<u64>,
    submitted_at: Option<u64>,
}

#[derive(Debug)]
struct Task {
    command: String,
    urgency: String,
    deadline: Option<u64>,
    started: bool,
    delayed: bool,
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    let file_appender = rolling::never("/tmp", "green_scheduler.log");
    let (non_blocking, _guard) = tracing_appender::non_blocking(file_appender);
    let env_filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));

    let fmt_layer = fmt::layer()
        .with_writer(non_blocking)
        .with_target(false)
        .with_thread_ids(false)
        .with_thread_names(false);

    if args.foreground {
        tracing_subscriber::registry()
            .with(env_filter)
            .with(fmt::layer())
            .init();
        info!("Starting in foreground mode");
    } else {
        tracing_subscriber::registry()
            .with(env_filter)
            .with(fmt_layer)
            .init();
        info!("Starting as daemon (logs to file)");

        if let Err(e) = write_pid(&args.pid) {
            warn!("Failed to write pid file {}: {}", args.pid, e);
        }
    }

    let client = Client::builder()
        .user_agent("green_scheduler/0.1 (rust)")
        .timeout(Duration::from_secs(10))
        .build()?;

    let tasks = Arc::new(RwLock::new(Vec::<Task>::new()));

    let pid_path = args.pid.clone();
    ctrlc::set_handler(move || {
        let _ = fs::remove_file(&pid_path);
        eprintln!("Termination signal received. PID file removed.");
        std::process::exit(0);
    })?;

    loop {
        if let Err(e) = reload_tasks(&args.config, tasks.clone()).await {
            warn!("Failed to reload tasks: {}", e);
        }

        let intensity = match fetch_carbon_intensity(&client).await {
            Ok(s) => s,
            Err(e) => {
                warn!("Failed to fetch carbon intensity: {}", e);
                String::from("unknown")
            }
        };

        info!("Current carbon intensity index = {}", intensity);

        let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();

        {
            let mut guard = tasks.write().await;
            for t in guard.iter_mut() {
                if t.started {
                    continue;
                }

                if t.urgency == "high" {
                    info!("Launching high-urgency task: {}", t.command);
                    if let Err(e) = launch_task(&t.command, &t.urgency).await {
                        error!("Failed to launch {}: {}", t.command, e);
                    } else {
                        t.started = true;
                    }
                    continue;
                }

                let high_bad = intensity == "high" || intensity == "very high";
                let deadline_passed = t.deadline.map_or(false, |d| d <= now);

                if high_bad && !deadline_passed {
                    t.delayed = true;
                    info!("Delaying task due to high intensity: {}", t.command);
                    continue;
                }

                info!("Launching task: {} (delayed={})", t.command, t.delayed);
                if let Err(e) = launch_task(&t.command, &t.urgency).await {
                    error!("Failed to launch {}: {}", t.command, e);
                } else {
                    t.started = true;
                }
            }
        }

        sleep(Duration::from_secs(args.interval)).await;
    }
}

async fn reload_tasks(path: &str, tasks: Arc<RwLock<Vec<Task>>>) -> Result<()> {
    let body = tokio::fs::read_to_string(path)
        .await
        .with_context(|| format!("reading config file {}", path))?;

    let arr: Vec<TaskConfig> = serde_json::from_str(&body).with_context(|| "parsing JSON")?;

    let mut new_tasks = Vec::with_capacity(arr.len());
    let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
    for t in arr.into_iter() {
        let submitted = t.submitted_at.unwrap_or(now);
        let deadline = t.deadline_hours.map(|h| submitted + h * 3600);
        new_tasks.push(Task {
            command: t.command,
            urgency: t.urgency,
            deadline,
            started: false,
            delayed: false,
        });
    }

    let mut guard = tasks.write().await;
    *guard = new_tasks;
    info!("Loaded {} tasks from {}", guard.len(), path);
    Ok(())
}

async fn fetch_carbon_intensity(client: &Client) -> Result<String> {
    let resp = client.get(CARBON_API_URL).send().await.context("request error")?;
    let json: serde_json::Value = resp.json().await.context("json parse")?;

    if let Some(index) = json
        .get("data")
        .and_then(|d| d.get(0))
        .and_then(|e| e.get("intensity"))
        .and_then(|i| i.get("index"))
        .and_then(|s| s.as_str())
    {
        return Ok(index.to_string());
    }

    Ok(String::from("unknown"))
}

async fn launch_task(command: &str, urgency: &str) -> Result<()> {
    let mut parts = shlex::split(command)
        .unwrap_or_else(|| command.split_whitespace().map(|s| s.to_string()).collect());
    if parts.is_empty() {
        anyhow::bail!("empty command");
    }
    let prog = parts.remove(0);

    let mut cmd = Command::new(prog);
    if !parts.is_empty() {
        cmd.args(parts);
    }

    if urgency == "low" {
        unsafe {
            cmd.pre_exec(|| {
                libc::nice(10);
                Ok(())
            });
        }
    }

    cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());

    let mut child = cmd.spawn().context("spawn task")?;

    tokio::spawn(async move {
        match child.wait().await {
            Ok(status) => info!("Task exited with status: {:?}", status),
            Err(e) => warn!("Failed waiting for task: {}", e),
        }
    });

    Ok(())
}

fn write_pid(path: &str) -> Result<()> {
    let pid = std::process::id();
    let mut f = File::create(path).context("create pid file")?;
    writeln!(f, "{}", pid).context("writing pid file")?;
    Ok(())
}
