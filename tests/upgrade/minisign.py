"""minisign's signature format, written with tests/upgrade/ed25519.py: for
tests and release dry runs, which sign with keys made for them (a real
release is signed with minisign itself).

    python3 tests/upgrade/minisign.py public <seed-hex> [<key-id-hex>]
    python3 tests/upgrade/minisign.py sign <seed-hex> <file> <trusted comment> [<key-id-hex>]

`public` prints the public key's line; `sign` writes <file>.minisig.
"""

import base64
import hashlib
import os
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519  # noqa: E402


class Key:
    def __init__(self, seed, key_id):
        self.secret = seed
        self.id = key_id
        self.public = base64.b64encode(b"Ed" + key_id + ed25519.public_key(seed)).decode()

    def sign(self, data, comment, prehashed=True):
        """The text of a .minisig file signing `data` (prehashed: as minisign
        has signed since 0.11, the BLAKE2b-512 hash of the file)."""
        message = hashlib.blake2b(data, digest_size=64).digest() if prehashed else data
        signature = ed25519.sign(self.secret, message)
        trusted = ed25519.sign(self.secret, signature + comment.encode())
        line = base64.b64encode((b"ED" if prehashed else b"Ed") + self.id + signature).decode()
        return (
            "untrusted comment: signature from a test key\n"
            + line
            + "\ntrusted comment: "
            + comment
            + "\n"
            + base64.b64encode(trusted).decode()
            + "\n"
        )


if __name__ == "__main__":
    command, rest = sys.argv[1], sys.argv[2:]
    if command == "public":
        key = Key(bytes.fromhex(rest[0]), bytes.fromhex(rest[1] if len(rest) > 1 else "0102030405060708"))
        print(key.public)
    elif command == "sign":
        key = Key(bytes.fromhex(rest[0]), bytes.fromhex(rest[3] if len(rest) > 3 else "0102030405060708"))
        with open(rest[1], "rb") as f:
            data = f.read()
        with open(rest[1] + ".minisig", "w", newline="\n") as f:
            f.write(key.sign(data, rest[2]))
    else:
        sys.exit(__doc__)
