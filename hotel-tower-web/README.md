# Hotel Tower — web edition

Browser version of the Hotel Universe promo game (three.js). Stack floors on
alternating axes from the street into space; floor 50 earns a promo code and
every floor beyond it a bigger one. Star tier every 20 floors (max 6) changes
the architecture; five perfect drops in a row auto-build 5 bonus floors.

## Run / deploy

Entirely static, no build step, no CDN dependencies (three.js is vendored in
`vendor/`). Serve the folder from any static host:

    python3 -m http.server 8641 --directory hotel-tower-web

or drop the folder onto Netlify/Vercel/S3/nginx as-is.

## Promo codes

Codes are `hu-` + 8 legible chars, derived from the floor count — identical
algorithm to the ESP32 build. Validate server-side with `../promo_codes.js`
(ESM) or `../promo_codes.py`; both recover the floor count from a code.

## QA

- `?debug=N` pre-builds N aligned floors (e.g. `/?debug=49` to test the
  floor-50 promo card with one drop, `/?debug=119` to test the 6-star tier).
- Best score is stored in `localStorage` under `hu_tower_best`.
- Input: tap / click / Space. `Enter` also confirms cards.
