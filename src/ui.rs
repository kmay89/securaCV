mod spinner {
    use std::io::{self, Write};
    use std::sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    };
    use std::thread::{self, JoinHandle};
    use std::time::Duration;

    #[derive(Clone, Debug)]
    pub struct ProgressDrawTarget;

    impl ProgressDrawTarget {
        pub fn stderr() -> Self {
            Self
        }
    }

    #[derive(Clone, Debug)]
    pub struct ProgressStyle {
        spinner: Vec<&'static str>,
    }

    #[derive(Clone, Debug)]
    pub struct TemplateError;

    impl ProgressStyle {
        pub fn with_template(_template: &str) -> Result<Self, TemplateError> {
            Ok(Self::default_spinner())
        }

        pub fn default_spinner() -> Self {
            Self {
                spinner: vec!["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"],
            }
        }
    }

    #[derive(Debug)]
    struct SpinnerState {
        running: AtomicBool,
        message: Mutex<String>,
        style: Mutex<ProgressStyle>,
    }

    #[derive(Debug)]
    pub struct ProgressBar {
        state: Arc<SpinnerState>,
        handle: Mutex<Option<JoinHandle<()>>>,
    }

    impl ProgressBar {
        pub fn new_spinner() -> Self {
            Self {
                state: Arc::new(SpinnerState {
                    running: AtomicBool::new(false),
                    message: Mutex::new(String::new()),
                    style: Mutex::new(ProgressStyle::default_spinner()),
                }),
                handle: Mutex::new(None),
            }
        }

        pub fn set_draw_target(&self, _target: ProgressDrawTarget) {}

        pub fn enable_steady_tick(&self, interval: Duration) {
            let mut handle = self.handle.lock().expect("spinner handle lock");
            if handle.is_some() {
                return;
            }
            self.state.running.store(true, Ordering::SeqCst);
            let state = Arc::clone(&self.state);
            let thread_handle = thread::spawn(move || {
                let mut idx = 0usize;
                while state.running.load(Ordering::SeqCst) {
                    let message = {
                        let guard = state.message.lock().expect("spinner message lock");
                        guard.clone()
                    };
                    let frames = {
                        let guard = state.style.lock().expect("spinner style lock");
                        guard.spinner.clone()
                    };
                    if !frames.is_empty() {
                        let frame = frames[idx % frames.len()];
                        idx = idx.wrapping_add(1);
                        let _ = write_line(&format!("{frame} {message}"));
                    }
                    thread::sleep(interval);
                }
            });
            *handle = Some(thread_handle);
        }

        pub fn set_style(&self, style: ProgressStyle) {
            let mut guard = self.state.style.lock().expect("spinner style lock");
            *guard = style;
        }

        pub fn set_message<S: Into<String>>(&self, message: S) {
            let mut guard = self.state.message.lock().expect("spinner message lock");
            *guard = message.into();
        }

        pub fn finish_with_message<S: Into<String>>(&self, message: S) {
            self.state.running.store(false, Ordering::SeqCst);
            if let Some(handle) = self.handle.lock().expect("spinner handle lock").take() {
                let _ = handle.join();
            }
            let message = message.into();
            let _ = write_line(&message);
            let _ = writeln!(io::stderr());
        }
    }

    fn write_line(text: &str) -> io::Result<()> {
        let mut err = io::stderr().lock();
        write!(err, "\r\x1b[2K{}", text)?;
        err.flush()
    }
}

use spinner::{ProgressBar, ProgressDrawTarget, ProgressStyle};
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
