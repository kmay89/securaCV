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

    #[allow(dead_code)]
    pub fn tick_strings(mut self, ticks: &'static [&'static str]) -> Self {
        self.spinner = ticks.to_vec();
        self
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
