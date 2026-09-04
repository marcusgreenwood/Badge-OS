// Hotel Tower promo code validation / parsing.
// Mirrors HotelTower/HotelTower.ino (and promo_codes.py): codes are
// "hu-" + 8 chars from a legible alphabet (no 0/1/i/l/o), derived from the
// floor count via splitmix64 with a fixed salt. One unique code per floor.
//
// Usage (ESM):
//   import { parsePromoCode } from "./promo_codes.js";
//   parsePromoCode("hu-nbem7c4y");  // -> { valid: true, floors: 50 }
//   parsePromoCode("hu-nope1234"); // -> { valid: false, floors: null }

const MASK = (1n << 64n) - 1n;
const ALPHA = "23456789abcdefghjkmnpqrstuvwxyz"; // 31 legible chars

const MIN_FLOORS = 50;  // first code is earned at floor 50
const MAX_FLOORS = 400; // the game's hard floor cap (kMaxFloors)

/** Compute the code for a floor count, e.g. 50 -> "hu-nbem7c4y". */
export function promoCode(floors) {
  let x =
    ((BigInt(floors) * 0x9e3779b97f4a7c15n) ^ 0x48554e4956455253n) & MASK;
  x ^= x >> 30n;
  x = (x * 0xbf58476d1ce4e5b9n) & MASK;
  x ^= x >> 27n;
  x = (x * 0x94d049bb133111ebn) & MASK;
  x ^= x >> 31n;
  let h = "";
  for (let i = 0; i < 8; i++) {
    h += ALPHA[Number(x % 31n)];
    x /= 31n;
  }
  return "hu-" + h;
}

// code -> floors, built once
const CODE_TABLE = new Map();
for (let f = MIN_FLOORS; f <= MAX_FLOORS; f++) {
  CODE_TABLE.set(promoCode(f), f);
}

/**
 * Validate a promo code and recover the floor count that earned it.
 * Tolerant of case, surrounding whitespace, and a missing "hu-" prefix.
 *
 * @param {string} input
 * @returns {{ valid: boolean, floors: number | null }}
 */
export function parsePromoCode(input) {
  if (typeof input !== "string") return { valid: false, floors: null };
  let code = input.trim().toLowerCase();
  if (!code.startsWith("hu-")) code = "hu-" + code;
  const floors = CODE_TABLE.get(code);
  return floors === undefined
    ? { valid: false, floors: null }
    : { valid: true, floors };
}

export { MIN_FLOORS, MAX_FLOORS };
