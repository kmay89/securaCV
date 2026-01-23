use indicatif::{ProgressBar, ProgressDrawTarget, ProgressStyle};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug)]
pub enum UiMode {
    Auto,
    Plain,
    Pretty,
}

#[derive(Clone, Debug)]
pub struct Ui {
    mode: UiMode,
    is_tty: bool,
    disable_pretty: bool,
}

impl Ui {
    pub fn new(mode: UiMode, is_tty: bool, disable_pretty: bool) -> Self {
        Self {
            mode,
            is_tty,
            disable_pretty,
        }
    }

    pub fn from_args(ui_flag: Option<&str>, is_tty: bool, disable_pretty: bool) -> Self {
        let mode = match ui_flag {
            Some("plain") => UiMode::Plain,
            Some("pretty") => UiMode::Pretty,
            _ => UiMode::Auto,
        };
        Self::new(mode, is_tty, disable_pretty)
    }

    pub fn stage(&self, name: &str) -> StageGuard {
        let use_pretty = self.is_tty
            && match self.mode {
                UiMode::Pretty => true,
                UiMode::Auto => !self.disable_pretty,
                UiMode::Plain => false,
            };

        if use_pretty {
            let spinner = ProgressBar::new_spinner();
            spinner.set_draw_target(ProgressDrawTarget::stderr());
            spinner.enable_steady_tick(Duration::from_millis(120));
            let style = ProgressStyle::with_template("{spinner} {msg}")
                .unwrap_or_else(|_| ProgressStyle::default_spinner());
            spinner.set_style(style);
            spinner.set_message(format!("{name}…"));
            StageGuard::new(name.to_string(), Some(spinner))
        } else {
            eprintln!("==> {}", name);
            StageGuard::new(name.to_string(), None)
        }
    }
}

pub struct StageGuard {
    name: String,
    start: Instant,
    spinner: Option<ProgressBar>,
}

impl StageGuard {
    fn new(name: String, spinner: Option<ProgressBar>) -> Self {
        Self {
            name,
            start: Instant::now(),
            spinner,
        }
    }
}

impl Drop for StageGuard {
    fn drop(&mut self) {
        let elapsed = self.start.elapsed();
        let message = format!("✔ {} ({})", self.name, format_duration(elapsed));
        if let Some(spinner) = &self.spinner {
            spinner.finish_with_message(message);
        } else {
            eprintln!("{message}");
        }
    }
}

fn format_duration(duration: Duration) -> String {
    if duration.as_secs() >= 1 {
        format!("{:.2}s", duration.as_secs_f64())
    } else {
        format!("{}ms", duration.as_millis())
    }
}
