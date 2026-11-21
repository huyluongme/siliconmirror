#!/usr/bin/env python3
# A reference model of Armv8.0 AArch64 floating point (FPCR.AH = 0, no
# FEAT_FP16 arithmetic, no trapped exceptions), written from the Arm ARM's
# shared pseudocode (J1.4.3) with exact rational arithmetic (fractions):
# FPUnpack, FPRound, FPProcessNaN(s), FPAdd/Sub/Mul/Div/MulAdd/Sqrt,
# FPMax/Min(Num), FPCompare, FPRoundInt, FPToFixed, FixedToFP, FPConvert.
# Independent of smirror's fp.c (which works on bits and sticky bits).
#
# Usage: fp_ref.py gen > hashes.inc   (the bulk test's expected hashes)
#        fp_ref.py check              (the model against the host FPU)
#        fp_ref.py dump GROUP [N]     (the cases of a group, for debugging)

import math
import struct
import sys
from fractions import Fraction

IOC, DZC, OFC, UFC, IXC, IDC = 1, 2, 4, 8, 16, 128
AHP, DN, FZ = 1 << 26, 1 << 25, 1 << 24
TIEEVEN, POSINF, NEGINF, ZERO, TIEAWAY, ODD = range(6)

PARAMS = {16: (5, 10, -14), 32: (8, 23, -126), 64: (11, 52, -1022)}


class Env:
    def __init__(self, fpcr):
        self.fpcr = fpcr
        self.flags = 0

    def rounding(self):
        return (self.fpcr >> 22) & 3


def unpack(env, n, x):
    """FPUnpackBase: (type, sign, value); FZ16 is 0."""
    E, F, min_exp = PARAMS[n]
    sign = (x >> (n - 1)) & 1
    exp = (x >> F) & ((1 << E) - 1)
    frac = x & ((1 << F) - 1)
    if exp == 0:
        if frac == 0 or (n != 16 and env.fpcr & FZ):
            if frac != 0:
                env.flags |= IDC
            return 'zero', sign, Fraction(0)
        return 'nonzero', sign, Fraction(frac, 1 << F) * Fraction(2) ** min_exp
    if exp == (1 << E) - 1 and not (n == 16 and env.fpcr & AHP):
        if frac == 0:
            return 'inf', sign, None
        return ('qnan' if frac >> (F - 1) else 'snan'), sign, Fraction(0)
    bias = (1 << (E - 1)) - 1
    return 'nonzero', sign, (1 + Fraction(frac, 1 << F)) * Fraction(2) ** (exp - bias)


def zero(n, sign):
    return sign << (n - 1)


def inf(n, sign):
    E, F, _ = PARAMS[n]
    return (sign << (n - 1)) | (((1 << E) - 1) << F)


def default_nan(n):
    E, F, _ = PARAMS[n]
    return (((1 << E) - 1) << F) | (1 << (F - 1))


def normalize(m):
    """NormalizeReal: (mantissa in [1, 2), exponent)."""
    e = m.numerator.bit_length() - m.denominator.bit_length()
    if m < Fraction(2) ** e:
        e -= 1
    mant = m / Fraction(2) ** e
    assert 1 <= mant < 2
    return mant, e


def fpround(env, n, op, rounding):
    """FPRoundBase with altfp FALSE, fpexc TRUE, satoflo FALSE."""
    assert op != 0
    E, F, min_exp = PARAMS[n]
    sign = 1 if op < 0 else 0
    mantissa, exponent = normalize(abs(op))
    if n != 16 and env.fpcr & FZ and exponent < min_exp:
        env.flags |= UFC
        return zero(n, sign)
    biased = max(exponent - min_exp + 1, 0)
    if biased == 0:
        mantissa = mantissa / Fraction(2) ** (min_exp - exponent)
    int_mant = math.floor(mantissa * 2 ** F)
    error = mantissa * 2 ** F - int_mant
    if biased == 0 and error != 0:
        env.flags |= UFC
    if rounding == TIEEVEN:
        up = error > Fraction(1, 2) or (error == Fraction(1, 2) and int_mant & 1)
        to_inf = True
    elif rounding == POSINF:
        up = error != 0 and sign == 0
        to_inf = sign == 0
    elif rounding == NEGINF:
        up = error != 0 and sign == 1
        to_inf = sign == 1
    else:  # ZERO, ODD
        up = False
        to_inf = False
    if up:
        int_mant += 1
        if int_mant == 1 << F:
            biased = 1
        if int_mant == 1 << (F + 1):
            biased += 1
            int_mant //= 2
    if error != 0 and rounding == ODD and int_mant & 1 == 0:
        int_mant += 1
    if n != 16 or not env.fpcr & AHP:
        if biased >= (1 << E) - 1:
            result = inf(n, sign) if to_inf else inf(n, sign) - 1
            env.flags |= OFC
            error = 1
        else:
            result = (sign << (n - 1)) | (biased << F) | (int_mant & ((1 << F) - 1))
    else:
        if biased >= 1 << E:
            result = (sign << (n - 1)) | ((1 << (n - 1)) - 1)
            env.flags |= IOC
            error = 0
        else:
            result = (sign << (n - 1)) | (biased << F) | (int_mant & ((1 << F) - 1))
    if error != 0:
        env.flags |= IXC
    return result


def process_nan(env, n, t, x):
    E, F, _ = PARAMS[n]
    if t == 'snan':
        x |= 1 << (F - 1)
        env.flags |= IOC
    if env.fpcr & DN:
        x = default_nan(n)
    return x


def process_nans(env, n, items):
    for want in ('snan', 'qnan'):
        for t, x in items:
            if t == want:
                return process_nan(env, n, t, x)
    return None


def value_of(t, s, v):
    return -v if s else v


def fp_add(env, n, a, b, sub=False):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    if sub:
        s2 ^= 1
    inf1, inf2 = t1 == 'inf', t2 == 'inf'
    z1, z2 = t1 == 'zero', t2 == 'zero'
    if inf1 and inf2 and s1 != s2:
        env.flags |= IOC
        return default_nan(n)
    if (inf1 and s1 == 0) or (inf2 and s2 == 0):
        return inf(n, 0)
    if (inf1 and s1 == 1) or (inf2 and s2 == 1):
        return inf(n, 1)
    if z1 and z2 and s1 == s2:
        return zero(n, s1)
    rv = value_of(t1, s1, v1) + value_of(t2, s2, v2)
    if rv == 0:
        return zero(n, 1 if env.rounding() == NEGINF else 0)
    return fpround(env, n, rv, env.rounding())


def fp_mul(env, n, a, b):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    if (t1 == 'inf' and t2 == 'zero') or (t1 == 'zero' and t2 == 'inf'):
        env.flags |= IOC
        return default_nan(n)
    if t1 == 'inf' or t2 == 'inf':
        return inf(n, s1 ^ s2)
    if t1 == 'zero' or t2 == 'zero':
        return zero(n, s1 ^ s2)
    return fpround(env, n, value_of(t1, s1, v1) * value_of(t2, s2, v2), env.rounding())


def fp_div(env, n, a, b):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    if (t1 == 'inf' and t2 == 'inf') or (t1 == 'zero' and t2 == 'zero'):
        env.flags |= IOC
        return default_nan(n)
    if t1 == 'inf' or t2 == 'zero':
        if t1 != 'inf':
            env.flags |= DZC
        return inf(n, s1 ^ s2)
    if t1 == 'zero' or t2 == 'inf':
        return zero(n, s1 ^ s2)
    return fpround(env, n, value_of(t1, s1, v1) / value_of(t2, s2, v2), env.rounding())


def fp_muladd(env, n, addend, a, b):
    ta, sa, va = unpack(env, n, addend)
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    inf1, inf2, z1, z2 = t1 == 'inf', t2 == 'inf', t1 == 'zero', t2 == 'zero'
    r = process_nans(env, n, [(ta, addend), (t1, a), (t2, b)])
    if ta == 'qnan' and ((inf1 and z2) or (z1 and inf2)):
        env.flags |= IOC
        return default_nan(n)
    if r is not None:
        return r
    infa, za = ta == 'inf', ta == 'zero'
    sp = s1 ^ s2
    infp, zp = inf1 or inf2, z1 or z2
    if (inf1 and z2) or (z1 and inf2) or (infa and infp and sa != sp):
        env.flags |= IOC
        return default_nan(n)
    if (infa and sa == 0) or (infp and sp == 0):
        return inf(n, 0)
    if (infa and sa == 1) or (infp and sp == 1):
        return inf(n, 1)
    if za and zp and sa == sp:
        return zero(n, sa)
    pv = 0 if zp else value_of(t1, s1, v1) * value_of(t2, s2, v2)
    rv = value_of(ta, sa, va) + pv
    if rv == 0:
        return zero(n, 1 if env.rounding() == NEGINF else 0)
    return fpround(env, n, rv, env.rounding())


def sqrt_fraction(v):
    """A rational that rounds as sqrt(v) does: exact, or the midpoint of an
    interval far narrower than any rounding needs."""
    k = 400
    num = v.numerator * v.denominator * 4 ** k  # sqrt(v) = sqrt(num)/(den*2^k)
    s = math.isqrt(num)
    den = v.denominator * 2 ** k
    if s * s == num:
        return Fraction(s, den)
    return Fraction(2 * s + 1, 2 * den)


def fp_sqrt(env, n, a):
    t, s, v = unpack(env, n, a)
    if t in ('snan', 'qnan'):
        return process_nan(env, n, t, a)
    if t == 'zero':
        return zero(n, s)
    if t == 'inf' and s == 0:
        return inf(n, 0)
    if s == 1:
        env.flags |= IOC
        return default_nan(n)
    return fpround(env, n, sqrt_fraction(v), env.rounding())


def big(t, s, v):
    """The value for comparisons: infinity as 2^1000000."""
    if t == 'inf':
        v = Fraction(2) ** 2000
    return -v if s else v


def fp_compare(env, n, a, b, signal):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    if t1 in ('snan', 'qnan') or t2 in ('snan', 'qnan'):
        if signal or t1 == 'snan' or t2 == 'snan':
            env.flags |= IOC
        return 0x3
    x, y = big(t1, s1, v1), big(t2, s2, v2)
    if x == y:
        return 0x6
    return 0x8 if x < y else 0x2


def fp_minmax(env, n, a, b, is_max):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    x, y = big(t1, s1, v1), big(t2, s2, v2)
    first = x > y if is_max else x < y
    t, s, v = (t1, s1, v1) if first else (t2, s2, v2)
    if t == 'inf':
        return inf(n, s)
    if t == 'zero':
        return zero(n, (s1 & s2) if is_max else (s1 | s2))
    return fpround(env, n, -v if s else v, env.rounding())


def fp_minmaxnum(env, n, a, b, is_max):
    quiet = Env(env.fpcr)
    t1 = unpack(quiet, n, a)[0]
    t2 = unpack(quiet, n, b)[0]
    if t1 == 'qnan' and t2 != 'qnan':
        a = inf(n, 1 if is_max else 0)
    elif t1 != 'qnan' and t2 == 'qnan':
        b = inf(n, 1 if is_max else 0)
    return fp_minmax(env, n, a, b, is_max)


def round_decision(rounding, int_result, error):
    """FPToFixed / FPRoundInt: whether to add 1 to RoundDown(value)."""
    half = Fraction(1, 2)
    if rounding == TIEEVEN:
        return error > half or (error == half and int_result & 1)
    if rounding == POSINF:
        return error != 0
    if rounding == NEGINF:
        return False
    if rounding == ZERO:
        return error != 0 and int_result < 0
    if rounding == TIEAWAY:
        return error > half or (error == half and int_result >= 0)
    raise ValueError(rounding)


def fp_round_int(env, n, a, rounding, exact):
    t, s, v = unpack(env, n, a)
    if t in ('snan', 'qnan'):
        return process_nan(env, n, t, a)
    if t == 'inf':
        return inf(n, s)
    if t == 'zero':
        return zero(n, s)
    value = -v if s else v
    int_result = math.floor(value)
    error = value - int_result
    if round_decision(rounding, int_result, error):
        int_result += 1
    if int_result == 0:
        result = zero(n, s)
    else:
        result = fpround(env, n, Fraction(int_result), ZERO)
    if error != 0 and exact:
        env.flags |= IXC
    return result


def fp_to_fixed(env, n, a, fbits, unsigned, rounding, m):
    t, s, v = unpack(env, n, a)
    if t in ('snan', 'qnan'):
        env.flags |= IOC
    if t == 'inf':
        v = Fraction(2) ** 3000
    value = (-v if s else v) * Fraction(2) ** fbits
    int_result = math.floor(value)
    error = value - int_result
    if round_decision(rounding, int_result, error):
        int_result += 1
    lo, hi = (0, (1 << m) - 1) if unsigned else (-(1 << (m - 1)), (1 << (m - 1)) - 1)
    if int_result < lo or int_result > hi:
        env.flags |= IOC
        int_result = min(max(int_result, lo), hi)
    elif error != 0:
        env.flags |= IXC
    return int_result & ((1 << m) - 1)


def fp_from_fixed(env, n, a, m, fbits, unsigned, rounding):
    a &= (1 << m) - 1
    if not unsigned and a >> (m - 1):
        a -= 1 << m
    value = Fraction(a) / Fraction(2) ** fbits
    if value == 0:
        return zero(n, 0)
    return fpround(env, n, value, rounding)


def convert_nan(to, frm, a):
    sign = (a >> (frm - 1)) & 1
    if frm == 64:
        frac = a & ((1 << 51) - 1)
    elif frm == 32:
        frac = (a & ((1 << 22) - 1)) << 29
    else:
        frac = (a & ((1 << 9) - 1)) << 42
    if to == 64:
        return (sign << 63) | (((1 << 12) - 1) << 51) | frac
    if to == 32:
        return (sign << 31) | (((1 << 9) - 1) << 22) | (frac >> 29)
    return (sign << 15) | (((1 << 6) - 1) << 9) | (frac >> 42)


def fp_convert(env, to, frm, a, rounding):
    t, s, v = unpack(env, frm, a)
    alt_hp = to == 16 and env.fpcr & AHP
    if t in ('snan', 'qnan'):
        if alt_hp:
            r = zero(to, s)
        elif env.fpcr & DN:
            r = default_nan(to)
        else:
            r = convert_nan(to, frm, a)
        if t == 'snan' or alt_hp:
            env.flags |= IOC
        return r
    if t == 'inf':
        if alt_hp:
            env.flags |= IOC
            return (s << (to - 1)) | ((1 << (to - 1)) - 1)
        return inf(to, s)
    if t == 'zero':
        return zero(to, s)
    return fpround(env, to, -v if s else v, rounding)


def bias_of(n):
    E, F, _ = PARAMS[n]
    return (1 << (E - 1)) - 1


def fp_two(n, sign):
    E, F, _ = PARAMS[n]
    return (sign << (n - 1)) | ((bias_of(n) + 1) << F)


def fp_onepointfive(n):
    E, F, _ = PARAMS[n]
    return (bias_of(n) << F) | (1 << (F - 1))


def fp_compare_eq(env, n, a, b):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    if t1 in ('snan', 'qnan') or t2 in ('snan', 'qnan'):
        if t1 == 'snan' or t2 == 'snan':
            env.flags |= IOC
        return False
    return big(t1, s1, v1) == big(t2, s2, v2)


def fp_compare_ge(env, n, a, b, gt=False):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    if t1 in ('snan', 'qnan') or t2 in ('snan', 'qnan'):
        env.flags |= IOC
        return False
    x, y = big(t1, s1, v1), big(t2, s2, v2)
    return x > y if gt else x >= y


def fp_mulx(env, n, a, b):
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    if (t1 == 'inf' and t2 == 'zero') or (t1 == 'zero' and t2 == 'inf'):
        return fp_two(n, s1 ^ s2)
    if t1 == 'inf' or t2 == 'inf':
        return inf(n, s1 ^ s2)
    if t1 == 'zero' or t2 == 'zero':
        return zero(n, s1 ^ s2)
    return fpround(env, n, value_of(t1, s1, v1) * value_of(t2, s2, v2), env.rounding())


def fp_step(env, n, a, b, rsqrt):
    a ^= 1 << (n - 1)
    t1, s1, v1 = unpack(env, n, a)
    t2, s2, v2 = unpack(env, n, b)
    r = process_nans(env, n, [(t1, a), (t2, b)])
    if r is not None:
        return r
    if (t1 == 'inf' and t2 == 'zero') or (t1 == 'zero' and t2 == 'inf'):
        return fp_onepointfive(n) if rsqrt else fp_two(n, 0)
    if t1 == 'inf' or t2 == 'inf':
        return inf(n, s1 ^ s2)
    prod = value_of(t1, s1, v1) * value_of(t2, s2, v2)
    rv = (3 + prod) / 2 if rsqrt else 2 + prod
    if rv == 0:
        return zero(n, 1 if env.rounding() == NEGINF else 0)
    return fpround(env, n, rv, env.rounding())


def recip_estimate_int(a):
    assert 256 <= a < 512
    a = a * 2 + 1
    b = (1 << 19) // a
    r = (b + 1) // 2
    assert 256 <= r < 512
    return r


def rsqrt_estimate_int(a):
    assert 128 <= a < 512
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    r = (b + 1) // 2
    assert 256 <= r < 512
    return r


def fields(n, a):
    E, F, _ = PARAMS[n]
    if n == 64:
        return a & ((1 << 52) - 1), (a >> 52) & 0x7ff
    if n == 32:
        return (a & ((1 << 23) - 1)) << 29, (a >> 23) & 0xff
    return (a & ((1 << 10) - 1)) << 42, (a >> 10) & 0x1f


def fp_recip_estimate(env, n, a):
    t, s, v = unpack(env, n, a)
    E, F, _ = PARAMS[n]
    if t in ('snan', 'qnan'):
        return process_nan(env, n, t, a)
    if t == 'inf':
        return zero(n, s)
    if t == 'zero':
        env.flags |= DZC
        return inf(n, s)
    lim = {16: -16, 32: -128, 64: -1024}[n]
    if v < Fraction(2) ** lim:
        rm = env.rounding()
        to_inf = {TIEEVEN: True, POSINF: s == 0, NEGINF: s == 1, ZERO: False}[rm]
        env.flags |= OFC | IXC
        return inf(n, s) if to_inf else inf(n, s) - 1
    hi = {16: 14, 32: 126, 64: 1022}[n]
    if n != 16 and env.fpcr & FZ and v >= Fraction(2) ** hi:
        env.flags |= UFC
        return zero(n, s)
    fraction, exp = fields(n, a)
    if exp == 0:
        if not (fraction >> 51) & 1:
            exp = -1
            fraction = (fraction & ((1 << 50) - 1)) << 2
        else:
            fraction = (fraction & ((1 << 51) - 1)) << 1
    scaled = 256 | (fraction >> 44)
    result_exp = {16: 29, 32: 253, 64: 2045}[n] - exp
    estimate = recip_estimate_int(scaled)
    fraction = (estimate & 0xff) << 44
    if result_exp == 0:
        fraction = (1 << 51) | (fraction >> 1)
    elif result_exp == -1:
        fraction = (1 << 50) | (fraction >> 2)
        result_exp = 0
    return (s << (n - 1)) | ((result_exp & ((1 << E) - 1)) << F) | (fraction >> (52 - F))


def fp_rsqrt_estimate(env, n, a):
    t, s, v = unpack(env, n, a)
    E, F, _ = PARAMS[n]
    if t in ('snan', 'qnan'):
        return process_nan(env, n, t, a)
    if t == 'zero':
        env.flags |= DZC
        return inf(n, s)
    if s == 1:
        env.flags |= IOC
        return default_nan(n)
    if t == 'inf':
        return zero(n, 0)
    fraction, exp = fields(n, a)
    if exp == 0:
        while not (fraction >> 51) & 1:
            fraction = (fraction << 1) & ((1 << 52) - 1)
            exp -= 1
        fraction = (fraction << 1) & ((1 << 52) - 1)
    if exp % 2 == 0:
        scaled = 256 | (fraction >> 44)
    else:
        scaled = 128 | (fraction >> 45)
    result_exp = ({16: 44, 32: 380, 64: 3068}[n] - exp) // 2
    estimate = rsqrt_estimate_int(scaled)
    return ((result_exp & ((1 << E) - 1)) << F) | ((estimate & 0xff) << (F - 8))


def fp_recpx(env, n, a):
    t, s, v = unpack(env, n, a)
    E, F, _ = PARAMS[n]
    exp = (a >> F) & ((1 << E) - 1)
    if t in ('snan', 'qnan'):
        return process_nan(env, n, t, a)
    if exp == 0:
        return (s << (n - 1)) | ((((1 << E) - 1) - 1) << F)
    return (s << (n - 1)) | (((~exp) & ((1 << E) - 1)) << F)


def unsigned_recip_estimate(a):
    if not (a >> 31) & 1:
        return 0xffffffff
    return recip_estimate_int(a >> 23) << 23


def unsigned_rsqrt_estimate(a):
    if (a >> 30) == 0:
        return 0xffffffff
    return rsqrt_estimate_int(a >> 23) << 23


# ---- The bulk test: inputs from a PRNG that the C test shares. ----

M64 = (1 << 64) - 1


class Rng:
    def __init__(self, seed):
        self.state = seed & M64

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & M64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


def operand(rng, n, near=None):
    """A value of n bits from a mix of classes; near: a value to stay close to."""
    E, F, _ = PARAMS[n]
    r = rng.next()
    sign = (r >> 8) & 1
    cls = r & 15
    frac = rng.next() & ((1 << F) - 1)
    emax = (1 << E) - 1
    if near is not None and (r >> 12) & 1:
        # Close to near, half the time: low bits changed, or the sign.
        x = near ^ ((rng.next() & 0xff) if cls < 8 else (1 << (n - 1)))
        if cls >= 8:
            x ^= rng.next() & 0xf
        return x & ((1 << n) - 1)
    if cls == 0:
        exp, frac = 0, 0
    elif cls == 1:
        exp, frac = emax, 0
    elif cls == 2:
        exp, frac = emax, frac | (1 << (F - 1))
    elif cls == 3:
        exp, frac = emax, (frac & ((1 << (F - 1)) - 1)) or 1
    elif cls == 4:
        exp = 0
    elif cls == 5:
        exp = 1 + (r >> 9) % 2
    elif cls == 6:
        exp = emax - 1 - (r >> 9) % 2
    elif cls == 7:
        exp = (emax >> 1) - 2 + (r >> 9) % 5
    else:
        return rng.next() & ((1 << n) - 1)
    return (sign << (n - 1)) | (exp << F) | frac


def int_operand(rng, m):
    r = rng.next()
    cls = r & 7
    x = rng.next()
    if cls == 0:
        x = 0
    elif cls == 1:
        x = 1 << (m - 1)
    elif cls == 2:
        x = (1 << (m - 1)) - 1
    elif cls == 3:
        x = x >> (rng.next() % m)
    elif cls == 4:
        x = -(x >> (rng.next() % m))
    return x & ((1 << m) - 1)


FPCRS = [0 << 22, 1 << 22, 2 << 22, 3 << 22, FZ, DN, FZ | DN | (1 << 22), AHP]
CASES = 1000


def groups():
    """(name, n, fpcr, function of (env, rng) giving the result)."""
    out = []
    for n in (32, 64):
        for fpcr in FPCRS[:7]:
            def two(fn, n=n):
                def run(env, rng):
                    a = operand(rng, n)
                    b = operand(rng, n, near=a)
                    return fn(env, n, a, b)
                return run
            out.append(('add', n, fpcr, two(fp_add)))
            out.append(('sub', n, fpcr, two(lambda e, n, a, b: fp_add(e, n, a, b, True))))
            out.append(('mul', n, fpcr, two(fp_mul)))
            out.append(('div', n, fpcr, two(fp_div)))
            out.append(('max', n, fpcr, two(lambda e, n, a, b: fp_minmax(e, n, a, b, True))))
            out.append(('min', n, fpcr, two(lambda e, n, a, b: fp_minmax(e, n, a, b, False))))
            out.append(('maxnum', n, fpcr, two(lambda e, n, a, b: fp_minmaxnum(e, n, a, b, True))))
            out.append(('minnum', n, fpcr, two(lambda e, n, a, b: fp_minmaxnum(e, n, a, b, False))))
            out.append(('cmp', n, fpcr, two(lambda e, n, a, b: fp_compare(e, n, a, b, False))))
            out.append(('cmpe', n, fpcr, two(lambda e, n, a, b: fp_compare(e, n, a, b, True))))

            def fma(env, rng, n=n):
                a = operand(rng, n)
                b = operand(rng, n)
                c = operand(rng, n)
                if rng.next() & 1:
                    # An addend near the product: cancellation.
                    p = fp_mul(Env(env.fpcr), n, a, b)
                    c = operand(rng, n, near=p ^ (1 << (n - 1)))
                return fp_muladd(env, n, c, a, b)
            out.append(('muladd', n, fpcr, fma))

            def sqrt(env, rng, n=n):
                return fp_sqrt(env, n, operand(rng, n))
            out.append(('sqrt', n, fpcr, sqrt))

            def rint(env, rng, n=n):
                mode = rng.next() % 7
                a = operand(rng, n)
                if mode < 5:
                    return fp_round_int(env, n, a, mode, False)
                return fp_round_int(env, n, a, env.rounding(), mode == 5)
            out.append(('rint', n, fpcr, rint))

            def tofix(env, rng, n=n):
                r = rng.next()
                m = 64 if r & 1 else 32
                unsigned = bool(r & 2)
                mode = (r >> 2) % 5
                fbits = 0 if r & 0x100 else (r >> 16) % (m + 1)
                return fp_to_fixed(env, n, operand(rng, n), fbits, unsigned, mode, m)
            out.append(('tofix', n, fpcr, tofix))

            def fromfix(env, rng, n=n):
                r = rng.next()
                m = 64 if r & 1 else 32
                unsigned = bool(r & 2)
                fbits = 0 if r & 0x100 else (r >> 16) % (m + 1)
                return fp_from_fixed(env, n, int_operand(rng, m), m, fbits, unsigned, env.rounding())
            out.append(('fromfix', n, fpcr, fromfix))
    for fpcr in FPCRS:
        for to, frm in ((32, 64), (64, 32), (16, 32), (16, 64), (32, 16), (64, 16)):
            def cvt(env, rng, to=to, frm=frm):
                return fp_convert(env, to, frm, operand(rng, frm), env.rounding())
            out.append(('cvt%dto%d' % (frm, to), 0, fpcr, cvt))
    # The operations of Advanced SIMD only; appended, so that the groups
    # above keep their seeds.
    for n in (32, 64):
        for fpcr in FPCRS[:7]:
            def two2(fn, n=n):
                def run(env, rng):
                    a = operand(rng, n)
                    b = operand(rng, n, near=a)
                    return fn(env, n, a, b)
                return run

            def one1(fn, n=n):
                def run(env, rng):
                    return fn(env, n, operand(rng, n))
                return run
            out.append(('mulx', n, fpcr, two2(fp_mulx)))
            out.append(('recps', n, fpcr, two2(lambda e, n, a, b: fp_step(e, n, a, b, False))))
            out.append(('rsqrts', n, fpcr, two2(lambda e, n, a, b: fp_step(e, n, a, b, True))))
            out.append(('cmeq', n, fpcr, two2(lambda e, n, a, b: int(fp_compare_eq(e, n, a, b)))))
            out.append(('cmge', n, fpcr, two2(lambda e, n, a, b: int(fp_compare_ge(e, n, a, b)))))
            out.append(('cmgt', n, fpcr, two2(lambda e, n, a, b: int(fp_compare_ge(e, n, a, b, True)))))
            out.append(('recpe', n, fpcr, one1(fp_recip_estimate)))
            out.append(('rsqrte', n, fpcr, one1(fp_rsqrt_estimate)))
            out.append(('recpx', n, fpcr, one1(fp_recpx)))
    for fpcr in FPCRS:
        def odd(env, rng):
            return fp_convert(env, 32, 64, operand(rng, 64), ODD)
        out.append(('cvtodd', 0, fpcr, odd))

    def urecpe(env, rng):
        return unsigned_recip_estimate(rng.next() & 0xffffffff)

    def ursqrte(env, rng):
        return unsigned_rsqrt_estimate(rng.next() & 0xffffffff)
    out.append(('urecpe', 32, 0, urecpe))
    out.append(('ursqrte', 32, 0, ursqrte))
    return out


def fnv(h, x):
    for i in range(8):
        h ^= (x >> (8 * i)) & 0xff
        h = (h * 0x100000001B3) & M64
    return h


def run_group(i, g, verbose=False, limit=None):
    name, n, fpcr, fn = g
    rng = Rng(0x534d4952 + i)
    h = 0xCBF29CE484222325
    for k in range(CASES):
        env = Env(fpcr)
        state = rng.state
        r = fn(env, rng)
        if verbose and (limit is None or k < limit):
            print('%4d state=0x%016x result=0x%x flags=0x%x' % (k, state, r, env.flags))
        h = fnv(h, r)
        h = fnv(h, env.flags)
    return h


def gen():
    print('/* Generated by fp_ref.py: %d cases a group. */' % CASES)
    for i, g in enumerate(groups()):
        name, n, fpcr, _ = g
        print('{"%s", %d, 0x%08x, UINT64_C(0x%016x)},' % (name, n, fpcr, run_group(i, g)))


# ---- The model against the host's FPU, rounding to nearest. ----

def host(n, x):
    if n == 64:
        return struct.unpack('<d', struct.pack('<Q', x))[0]
    return struct.unpack('<f', struct.pack('<I', x))[0]


def bits(n, v):
    if n == 64:
        return struct.unpack('<Q', struct.pack('<d', v))[0]
    return struct.unpack('<I', struct.pack('<f', v))[0]


def check():
    rng = Rng(1)
    bad = 0
    count = 0
    for n in (32, 64):
        for k in range(20000):
            a = operand(rng, n)
            b = operand(rng, n, near=a)
            for name, fn, hf in (
                    ('add', fp_add, lambda x, y: x + y),
                    ('sub', lambda e, n, a, b: fp_add(e, n, a, b, True), lambda x, y: x - y),
                    ('mul', fp_mul, lambda x, y: x * y),
                    ('div', fp_div, lambda x, y: x / y if y != 0 else None),
                    ('sqrt', lambda e, n, a, b: fp_sqrt(e, n, a), lambda x, y: math.sqrt(x) if x >= 0 else None)):
                x, y = host(n, a), host(n, b)
                if math.isnan(x) or math.isnan(y):
                    continue
                try:
                    hv = hf(x, y)
                except (OverflowError, ZeroDivisionError):
                    continue
                if hv is None or math.isinf(hv) or math.isnan(hv):
                    continue
                if n == 32:
                    try:
                        hb = bits(32, hv)
                    except OverflowError:
                        continue
                else:
                    hb = bits(64, hv)
                if (hb >> (n - 1 - (8 if n == 32 else 11))) & ((1 << (8 if n == 32 else 11)) - 1) == ((1 << (8 if n == 32 else 11)) - 1):
                    continue  # rounded to infinity on the host: skip
                mine = fn(Env(0), n, a, b)
                count += 1
                if mine != hb:
                    bad += 1
                    if bad < 10:
                        print('%s%d 0x%x 0x%x: model 0x%x host 0x%x' % (name, n, a, b, mine, hb))
    print('%d compared, %d differ' % (count, bad))
    return bad == 0


if __name__ == '__main__':
    if sys.argv[1] == 'gen':
        gen()
    elif sys.argv[1] == 'check':
        sys.exit(0 if check() else 1)
    elif sys.argv[1] == 'dump':
        gs = groups()
        i = [k for k, g in enumerate(gs) if '%s/%d/0x%08x' % (g[0], g[1], g[2]) == sys.argv[2]][0]
        run_group(i, gs[i], True, int(sys.argv[3]) if len(sys.argv) > 3 else None)
