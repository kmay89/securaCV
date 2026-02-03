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

The SecuraCV brand represents:
- **Shield**: Security and protection
- **Eye**: Witness/observation capability
- **Chain links**: Hash chain integrity (blockchain-style verification)
- **Color palette**: Deep teal (#1a5f7a, #2d7d9a) for trust and security

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

The `icon.svg` and `logo.svg` files are the source vectors if you need to modify the design.
