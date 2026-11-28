"""Ed25519 signing as RFC 8032 (section 6) gives it, in plain Python: the
tests sign releases with it, independently of the Monocypher code that
checks them. Slow, and not constant-time: for tests only."""

import hashlib

p = 2**255 - 19
q = 2**252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, p - 2, p)


d = -121665 * _inv(121666) % p
_sqrt_m1 = pow(2, (p - 1) // 4, p)


def _sha512_modq(data):
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % q


def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F, G * H, F * G, E * H)


def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _recover_x(y, sign):
    x2 = (y * y - 1) * _inv(d * y * y + 1)
    if x2 == 0:
        return 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * _sqrt_m1 % p
    if (x & 1) != sign:
        x = p - x
    return x


_gy = 4 * _inv(5) % p
_gx = _recover_x(_gy, 0)
_G = (_gx, _gy, 1, _gx * _gy % p)


def _compress(P):
    zinv = _inv(P[2])
    x, y = P[0] * zinv % p, P[1] * zinv % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _expand(secret):
    h = hashlib.sha512(secret).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(secret):
    """The 32-byte public key of a 32-byte secret key (seed)."""
    return _compress(_mul(_expand(secret)[0], _G))


def sign(secret, message):
    """The 64-byte signature of `message` with a 32-byte secret key."""
    a, prefix = _expand(secret)
    A = _compress(_mul(a, _G))
    r = _sha512_modq(prefix + message)
    R = _compress(_mul(r, _G))
    s = (r + _sha512_modq(R + A + message) * a) % q
    return R + int.to_bytes(s, 32, "little")
