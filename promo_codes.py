#!/usr/bin/env python3
"""Hotel Tower promo code generator / validator.

Mirrors the algorithm in HotelTower/HotelTower.ino: codes are hu-{hash},
8 chars from a legible alphabet (no 0/1/i/l/o), derived from the floor
count with splitmix64 and a fixed salt. One code per floor >= 50.

Usage:
  python3 promo_codes.py            # print codes for floors 50..100
  python3 promo_codes.py 50 200     # print codes for floors 50..200
"""
import sys

MASK = (1 << 64) - 1
ALPHA = "23456789abcdefghjkmnpqrstuvwxyz"  # 31 legible chars


def promo_code(floors: int) -> str:
    x = ((floors * 0x9E3779B97F4A7C15) ^ 0x48554E4956455253) & MASK
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & MASK
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & MASK
    x ^= x >> 31
    h = ""
    for _ in range(8):
        h += ALPHA[x % 31]
        x //= 31
    return f"hu-{h}"


if __name__ == "__main__":
    lo = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    hi = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    for f in range(lo, hi + 1):
        print(f"{f}\t{promo_code(f)}")
