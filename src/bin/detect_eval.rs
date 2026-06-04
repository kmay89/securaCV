//! `detect_eval` — measure perception quality for a detector backend against a labeled dataset.
//!
//! The kernel's crypto proves a sealed event was not forged; this binary measures whether the
//! detector actually *saw* the event in the first place — precision, recall, F1, average
//! precision, a confidence-threshold sweep, size-class confusion, and per-frame latency.
//!
//! Build with `--features detect-eval`. Fetch a model first with
//! `bash scripts/fetch_detection_model.sh`, or point `--model` at any compatible ONNX file.
//!
//! ```text
//! cargo run --features detect-eval --bin detect_eval -- \
//!     --model vendor/models/tinyyolov2-8.onnx \
//!     --dataset eval/datasets/sample --json report.json
//! ```

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{anyhow, Result};
use clap::Parser;

use witness_kernel::config::TractFormat;
use witness_kernel::eval::metrics::EvalReport;
use witness_kernel::eval::{parse_sweep_spec, run_eval, EvalConfig};

/// Default model path written by `scripts/fetch_detection_model.sh`.
const DEFAULT_MODEL: &str = "vendor/models/tinyyolov2-8.onnx";

#[derive(Parser, Debug)]
#[command(
    name = "detect_eval",
    about = "Measure detection precision/recall/latency against a labeled dataset"
)]
struct Args {
    /// Path to the ONNX model file.
    #[arg(long, default_value = DEFAULT_MODEL)]
    model: PathBuf,

    /// Dataset directory containing labels.json + images.
    #[arg(long)]
    dataset: PathBuf,

    /// Output format: "yolov2" (raw grid decoded on host — the bundled tiny-YOLOv2) or "postnms"
    /// (model already emits final boxes).
    #[arg(long, default_value = "yolov2")]
    format: String,

    /// Model input width (PostNms only; YOLOv2 has a fixed 416×416 input).
    #[arg(long, default_value_t = 416)]
    width: u32,

    /// Model input height (PostNms only; YOLOv2 has a fixed 416×416 input).
    #[arg(long, default_value_t = 416)]
    height: u32,

    /// IoU threshold for matching a prediction to ground truth.
    #[arg(long, default_value_t = 0.5)]
    iou_threshold: f32,

    /// Operating confidence threshold for the headline metrics.
    #[arg(long, default_value_t = 0.5)]
    threshold: f32,

    /// Confidence sweep as start:end:step.
    #[arg(long, default_value = "0.05:0.95:0.05")]
    confidence_sweep: String,

    /// Write the full report as JSON to this path.
    #[arg(long)]
    json: Option<PathBuf>,

    /// Fail (non-zero exit) if overall recall is below this value.
    #[arg(long)]
    min_recall: Option<f32>,

    /// Fail (non-zero exit) if overall precision is below this value.
    #[arg(long)]
    min_precision: Option<f32>,
}

fn print_report(report: &EvalReport) {
    println!("\nPerception eval — {} ({})", report.model, report.backend);
    println!(
        "frames={}  ground_truth={}  predictions={}  iou>={:.2}  operating_conf>={:.2}",
        report.frames,
        report.ground_truth_objects,
        report.predictions,
        report.iou_threshold,
        report.operating_threshold,
    );

    println!("\nPer-class (at operating threshold):");
    println!(
        "  {:<9} {:>4} {:>4} {:>4} {:>4} {:>7} {:>7} {:>7} {:>7}",
        "class", "gt", "tp", "fp", "fn", "prec", "recall", "f1", "ap"
    );
    for c in &report.per_class {
        let ap = c
            .average_precision
            .map(|v| format!("{v:.3}"))
            .unwrap_or_else(|| "  n/a".to_string());
        println!(
            "  {:<9} {:>4} {:>4} {:>4} {:>4} {:>7.3} {:>7.3} {:>7.3} {:>7}",
            c.class,
            c.ground_truth,
            c.counts.true_positives,
            c.counts.false_positives,
            c.counts.false_negatives,
            c.precision,
            c.recall,
            c.f1,
            ap,
        );
    }

    println!("\nOverall (micro):");
    println!(
        "  precision={:.3}  recall={:.3}  f1={:.3}  mAP={:.3}  witness_miss_rate={:.3}",
        report.overall.precision,
        report.overall.recall,
        report.overall.f1,
        report.mean_average_precision,
        report.overall.witness_miss_rate,
    );

    println!("\nConfidence sweep (micro precision/recall/f1):");
    println!("  {:>6} {:>7} {:>7} {:>7}", "conf", "prec", "recall", "f1");
    for p in &report.confidence_sweep {
        println!(
            "  {:>6.2} {:>7.3} {:>7.3} {:>7.3}",
            p.threshold, p.precision, p.recall, p.f1
        );
    }
    // Recommend the threshold that maximizes micro F1 — a data-driven alternative to 0.5.
    if let Some(best) = report
        .confidence_sweep
        .iter()
        .max_by(|a, b| a.f1.total_cmp(&b.f1))
    {
        println!(
            "  -> best micro-F1 {:.3} at confidence {:.2}",
            best.f1, best.threshold
        );
    }

    if report.size_class_confusion.total > 0 {
        println!(
            "\nSize-class accuracy: {:.3} ({}/{} frames)",
            report.size_class_confusion.accuracy,
            report.size_class_confusion.correct,
            report.size_class_confusion.total,
        );
        for cell in &report.size_class_confusion.cells {
            println!(
                "  expected {:<7} -> predicted {:<7}: {}",
                cell.expected, cell.predicted, cell.count
            );
        }
    }

    let l = &report.latency;
    println!(
        "\nLatency (ms): mean={:.2} p50={:.2} p95={:.2} p99={:.2} max={:.2} (n={})",
        l.mean_ms, l.p50_ms, l.p95_ms, l.p99_ms, l.max_ms, l.count
    );

    // A loud, plain-language summary tied to the product's actual promise.
    println!(
        "\nWITNESS SUMMARY: at conf>={:.2}, this detector witnessed {:.1}% of labeled events \
         and raised false alarms on {:.1}% of its detections.",
        report.operating_threshold,
        report.overall.recall * 100.0,
        (1.0 - report.overall.precision) * 100.0,
    );
}

fn run() -> Result<ExitCode> {
    let args = Args::parse();

    let sweep = parse_sweep_spec(&args.confidence_sweep).map_err(|e| anyhow!(e))?;
    if !(0.0..=1.0).contains(&args.iou_threshold) || !(0.0..=1.0).contains(&args.threshold) {
        return Err(anyhow!("iou-threshold and threshold must be within 0..=1"));
    }
    let format = TractFormat::parse(&args.format)?;

    let cfg = EvalConfig {
        model_path: args.model,
        dataset_dir: args.dataset,
        width: args.width,
        height: args.height,
        format,
        iou_threshold: args.iou_threshold,
        operating_threshold: args.threshold,
        sweep,
    };

    let report = run_eval(&cfg)?;
    print_report(&report);

    if let Some(path) = &args.json {
        let json = serde_json::to_string_pretty(&report)?;
        std::fs::write(path, json)?;
        println!("\nWrote report to {}", path.display());
    }

    let mut failed = false;
    if let Some(min) = args.min_recall {
        if report.overall.recall < min {
            eprintln!(
                "FAIL: overall recall {:.3} < required {:.3}",
                report.overall.recall, min
            );
            failed = true;
        }
    }
    if let Some(min) = args.min_precision {
        if report.overall.precision < min {
            eprintln!(
                "FAIL: overall precision {:.3} < required {:.3}",
                report.overall.precision, min
            );
            failed = true;
        }
    }

    Ok(if failed {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    })
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(e) => {
            eprintln!("error: {e:#}");
            ExitCode::FAILURE
        }
    }
}
