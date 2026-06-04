//! Label-manifest types and loader for the perception eval harness.
//!
//! A dataset is a directory containing a `labels.json` manifest plus the referenced image
//! files. The box schema deliberately mirrors `detect::Detection` (normalized 0..1 `x,y,w,h`)
//! so there is no coordinate-convention drift between ground truth and predictions.

use std::path::Path;

use anyhow::{anyhow, Context, Result};
use serde::Deserialize;

use crate::detect::{ObjectClass, SizeClass};
use crate::eval::metrics::{EvalBox, GtObject};

/// Top-level manifest: the list of labeled frames.
#[derive(Debug, Clone, Deserialize)]
pub struct LabelManifest {
    pub frames: Vec<FrameLabel>,
}

/// One labeled frame.
#[derive(Debug, Clone, Deserialize)]
pub struct FrameLabel {
    /// Image filename, resolved relative to the dataset directory.
    pub image: String,
    #[serde(default)]
    pub objects: Vec<ObjectLabel>,
    /// Optional expected per-frame size class (`small` | `large` | `unknown`). When present, it
    /// feeds the size-class confusion matrix.
    #[serde(default)]
    pub expected_size_class: Option<String>,
}

/// One labeled object (normalized box + class).
#[derive(Debug, Clone, Deserialize)]
pub struct ObjectLabel {
    pub class: String,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl ObjectLabel {
    pub fn to_gt(&self) -> Result<GtObject> {
        Ok(GtObject {
            class: parse_class(&self.class)?,
            bbox: EvalBox::new(self.x, self.y, self.w, self.h),
        })
    }
}

impl FrameLabel {
    pub fn ground_truth(&self) -> Result<Vec<GtObject>> {
        self.objects.iter().map(ObjectLabel::to_gt).collect()
    }

    pub fn expected_size(&self) -> Result<Option<SizeClass>> {
        self.expected_size_class
            .as_deref()
            .map(parse_size_class)
            .transpose()
    }
}

/// Parse a class label (lowercase) into an [`ObjectClass`].
pub fn parse_class(s: &str) -> Result<ObjectClass> {
    match s.trim().to_ascii_lowercase().as_str() {
        "person" => Ok(ObjectClass::Person),
        "vehicle" => Ok(ObjectClass::Vehicle),
        "animal" => Ok(ObjectClass::Animal),
        "package" => Ok(ObjectClass::Package),
        "unknown" => Ok(ObjectClass::Unknown),
        other => Err(anyhow!(
            "unknown object class '{other}' (expected person|vehicle|animal|package|unknown)"
        )),
    }
}

/// Parse a size-class label (lowercase) into a [`SizeClass`].
pub fn parse_size_class(s: &str) -> Result<SizeClass> {
    match s.trim().to_ascii_lowercase().as_str() {
        "small" => Ok(SizeClass::Small),
        "large" => Ok(SizeClass::Large),
        "unknown" => Ok(SizeClass::Unknown),
        other => Err(anyhow!(
            "unknown size class '{other}' (expected small|large|unknown)"
        )),
    }
}

/// Load and parse `labels.json` from a dataset directory.
pub fn load_manifest(dataset_dir: &Path) -> Result<LabelManifest> {
    let path = dataset_dir.join("labels.json");
    let text = std::fs::read_to_string(&path)
        .with_context(|| format!("reading label manifest {}", path.display()))?;
    let manifest: LabelManifest = serde_json::from_str(&text)
        .with_context(|| format!("parsing label manifest {}", path.display()))?;
    if manifest.frames.is_empty() {
        return Err(anyhow!("label manifest {} has no frames", path.display()));
    }
    Ok(manifest)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_classes_case_insensitively() {
        assert_eq!(parse_class("Person").unwrap(), ObjectClass::Person);
        assert_eq!(parse_class(" vehicle ").unwrap(), ObjectClass::Vehicle);
        assert!(parse_class("face").is_err());
    }

    #[test]
    fn parses_size_classes() {
        assert_eq!(parse_size_class("large").unwrap(), SizeClass::Large);
        assert!(parse_size_class("medium").is_err());
    }

    #[test]
    fn deserializes_manifest() {
        let json = r#"{
            "frames": [
                {"image": "a.png",
                 "objects": [{"class": "person", "x": 0.1, "y": 0.2, "w": 0.3, "h": 0.4}],
                 "expected_size_class": "small"}
            ]
        }"#;
        let m: LabelManifest = serde_json::from_str(json).unwrap();
        assert_eq!(m.frames.len(), 1);
        let gt = m.frames[0].ground_truth().unwrap();
        assert_eq!(gt.len(), 1);
        assert_eq!(gt[0].class, ObjectClass::Person);
        assert_eq!(m.frames[0].expected_size().unwrap(), Some(SizeClass::Small));
    }
}
