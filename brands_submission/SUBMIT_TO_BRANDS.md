# Submitting SecuraCV to Home Assistant Brands Repository

This folder contains the brand assets ready for submission to the [home-assistant/brands](https://github.com/home-assistant/brands) repository.

## Assets Included

| File | Dimensions | Description |
|------|-----------|-------------|
| `icon.png` | 256x256 | Square icon (standard resolution) |
| `icon@2x.png` | 512x512 | Square icon (high DPI) |
| `logo.png` | 256x256 | Logo (standard resolution) |
| `logo@2x.png` | 512x512 | Logo (high DPI) |

All images are PNG format with RGBA transparency.

## Submission Steps

### 1. Fork the brands repository

```bash
gh repo fork home-assistant/brands --clone
cd brands
```

### 2. Create your branch

```bash
git checkout -b add-securacv-brand
```

### 3. Copy the assets

```bash
mkdir -p custom_integrations/securacv
cp /path/to/securaCV/brands_submission/custom_integrations/securacv/* custom_integrations/securacv/
```

### 4. Commit and push

```bash
git add custom_integrations/securacv/
git commit -m "Add SecuraCV custom integration brand"
git push -u origin add-securacv-brand
```

### 5. Create the Pull Request

```bash
gh pr create --title "Add SecuraCV custom integration brand" --body "Adds brand assets for SecuraCV HACS integration.

## Integration Details
- **Domain:** securacv
- **Repository:** https://github.com/kmay89/securaCV
- **HACS:** Available in HACS default repository

## Assets Added
- icon.png (256x256)
- icon@2x.png (512x512)
- logo.png (256x256)
- logo@2x.png (512x512)

All images meet the [brand requirements](https://github.com/home-assistant/brands#readme)."
```

## Requirements Checklist

- [x] Icons are square (1:1 aspect ratio)
- [x] icon.png is 256x256 pixels
- [x] icon@2x.png is 512x512 pixels
- [x] logo.png is 256x256 pixels
- [x] logo@2x.png is 512x512 pixels
- [x] All images are PNG format
- [x] All images have transparency
- [x] No Home Assistant branded images used

## After Merge

Once your PR is merged to home-assistant/brands:

1. The brand will automatically be available in Home Assistant
2. Your integration's icon and logo will display properly in:
   - Home Assistant UI
   - HACS integration list
   - Developer tools
   - Integration settings pages
