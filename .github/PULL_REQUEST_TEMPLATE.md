## What Changed

<!-- Brief description of changes -->

## Checklist

### Required
- [ ] Firmware compiles in **both** Arduino IDE and PlatformIO
- [ ] `regression_check.sh` passes with no errors
- [ ] No new hardcoded secrets or default passwords
- [ ] All new UI buttons have working API endpoints
- [ ] All new API endpoints return proper JSON with error codes

### If you touched crypto/auth:
- [ ] Token comparison uses constant-time function
- [ ] No raw key material in logs, SD, or witness chain
- [ ] Key derivation uses domain separation

### If you touched camera/GPS/SD:
- [ ] Correct XIAO ESP32S3 Sense pin definitions
- [ ] Graceful degradation if hardware absent

### If you touched web_ui.h:
- [ ] No `localStorage` / `sessionStorage` / `document.cookie`
- [ ] File size under 64KB
- [ ] Every new button has a backend handler

### Lessons Learned
- [ ] If this PR fixes a bug, add an entry to `firmware/LESSONS_LEARNED.md`
- [ ] If this PR introduces a new pattern, document it in LESSONS_LEARNED
