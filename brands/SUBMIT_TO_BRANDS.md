# Submitting SecuraCV to Home Assistant Brands

This folder contains brand assets for the SecuraCV integration ready for submission to the [home-assistant/brands](https://github.com/home-assistant/brands) repository.

## Files Included

| File | Dimensions | Purpose |
|------|------------|---------|
| `icon.png` | 256×256 | Standard resolution icon |
| `icon@2x.png` | 512×512 | High DPI icon |
| `logo.png` | 512×180 | Standard resolution logo |
| `logo@2x.png` | 1024×360 | High DPI logo |

## Submission Steps

1. **Fork the brands repository**
   ```bash
   gh repo fork home-assistant/brands --clone
   cd brands
   ```

2. **Create the custom integration folder**
   ```bash
   mkdir -p custom_integrations/securacv
   ```

3. **Copy the brand assets**
   ```bash
   cp /path/to/securaCV/brands/*.png custom_integrations/securacv/
   ```

4. **Commit and push**
   ```bash
   git checkout -b add-securacv-brand
   git add custom_integrations/securacv/
   git commit -m "Add SecuraCV custom integration brand"
   git push origin add-securacv-brand
   ```

5. **Create Pull Request**
   ```bash
   gh pr create --title "Add SecuraCV custom integration brand" --body "Adding brand assets for SecuraCV - Privacy Witness System for Home Assistant.

   - Domain: securacv
   - Repository: https://github.com/kmay89/securaCV
   - HACS: Yes"
   ```

## Brand Design

The SecuraCV brand features the **Canary mascot** - a friendly yellow canary with a notepad, representing:
- **Canary**: The "canary in the coal mine" - an early warning witness that documents events
- **Notepad**: Recording and witnessing capability - documenting what happens
- **Bright yellow**: Visibility, alertness, and approachability
- **Friendly expression**: Privacy-respecting, non-threatening surveillance alternative

The canary mascot embodies the project's core mission: being a trustworthy witness that
documents events for accountability while respecting privacy.

## After PR is Merged

Once your brands PR is merged to home-assistant/brands:

1. Update `.github/workflows/validate.yml` to remove the `ignore: brands` line:
   ```yaml
   - uses: hacs/action@main
     with:
       category: integration
       # Remove the 'ignore: brands' line
   ```

2. Commit and push:
   ```bash
   git add .github/workflows/validate.yml
   git commit -m "Remove brands ignore after brands PR merged"
   git push
   ```

3. The HACS validation will now pass the brands check
4. SecuraCV will display with proper branding in HACS

## Source Files

- `logo_main.png` - The master canary mascot image (1024x1024)
- `generate_brand_assets.py` - Script to regenerate all sizes from the source image

To regenerate the brand assets after modifying the source:
```bash
python3 generate_brand_assets.py logo_main.png
```
