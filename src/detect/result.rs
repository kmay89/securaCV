/// Result of running detection on a frame.
#[derive(Clone, Debug, Default)]
pub struct DetectionResult {
    /// Did we detect motion/presence?
    pub motion_detected: bool,
    /// Bounding boxes (normalized 0..1 coordinates).
    pub detections: Vec<Detection>,
    /// Confidence of primary detection.
    pub confidence: f32,
    /// Size class (large/small object).
    pub size_class: SizeClass,
}

#[derive(Clone, Debug)]
pub struct Detection {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub confidence: f32,
    pub class: ObjectClass,
}

#[non_exhaustive]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObjectClass {
    Person,
    Vehicle,
    Animal,
    Package,
    Unknown,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SizeClass {
    #[default]
    Unknown,
    Small,
    Large,
}
