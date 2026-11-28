"""`cluau upgrade` against releases made here: a copy of the cluau under
test, installed in a temporary prefix, upgrades from a local directory
holding a release signed in minisign's format (with tests/upgrade/ed25519.py,
not the code that checks it). Then releases that must be refused, each
leaving the installed command as it was.

    python3 tests/upgrade/run.py <cluau>
"""

import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minisign import Key  # noqa: E402

cluau = os.path.abspath(sys.argv[1])
windows = os.name == "nt"
program = "cluau.exe" if windows else "cluau"


def run(command, env=None):
    full = dict(os.environ)
    for name in ("CLUAU_REPOSITORY", "CLUAU_DOWNLOAD_URL", "CLUAU_RELEASE_KEYS"):
        full.pop(name, None)
    full.update(env or {})
    return subprocess.run(command, capture_output=True, text=True, env=full, timeout=120)


def query(code):
    out = run([cluau, "-e", code])
    assert out.returncode == 0, out.stderr
    return out.stdout.strip()


version = query('print(require("@std/_release").version)')
platform = query('print(require("@std/_release").target)')
tag = "v" + version


key = Key(bytes(range(32)), bytes.fromhex("0102030405060708"))
other = Key(bytes(range(1, 33)), bytes.fromhex("1112131415161718"))

root = tempfile.mkdtemp(prefix="cluau-upgrade-test-")
failures = []


def check(name, condition, detail=""):
    if condition:
        print("ok  ", name)
    else:
        print("FAIL", name, detail)
        failures.append(name)


def archive_bytes(folder, files):
    """The release archive holding `files` ({name: bytes}) under `folder`."""
    buffer = io.BytesIO()
    if windows:
        with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in files.items():
                z.writestr(f"{folder}/{name}", data)
    else:
        with tarfile.open(fileobj=buffer, mode="w:gz") as t:
            for name, data in files.items():
                info = tarfile.TarInfo(f"{folder}/{name}")
                info.size = len(data)
                info.mode = 0o755
                t.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def make_release(name, release_tag=None, command=None, readme=b"the new README\n", sign_with=None,
                 comment=None, prehashed=True, signed=True, tamper_sums=False, tamper_comment=False,
                 tamper_archive=False):
    release_tag = release_tag or tag
    directory = os.path.join(root, "releases", name)
    os.makedirs(directory)
    with open(cluau, "rb") as f:
        binary = f.read()
    folder = f"cluau-{release_tag}-{platform}"
    data = archive_bytes(folder, {program: command if command is not None else binary, "README.md": readme})
    archive = f"{folder}.{'zip' if windows else 'tar.gz'}"
    sums = f"{hashlib.sha256(data).hexdigest()}  {archive}\n".encode()
    if tamper_archive:
        data = data[:-1] + bytes([data[-1] ^ 1])
    with open(os.path.join(directory, archive), "wb") as f:
        f.write(data)
    signature = (sign_with or key).sign(sums, comment or f"cluau {release_tag}", prehashed)
    if tamper_sums:
        sums += b"0000000000000000000000000000000000000000000000000000000000000000  extra\n"
    if tamper_comment:
        signature = signature.replace("\ntrusted comment: ", "\ntrusted comment: x", 1)
    with open(os.path.join(directory, "SHA256SUMS"), "wb") as f:
        f.write(sums)
    if signed:
        with open(os.path.join(directory, "SHA256SUMS.minisig"), "w", newline="\n") as f:
            f.write(signature)
    return directory


def install(prefix):
    bin_dir = os.path.join(prefix, "bin")
    doc_dir = os.path.join(prefix, "doc") if windows else os.path.join(prefix, "share", "doc", "cluau")
    os.makedirs(bin_dir)
    os.makedirs(doc_dir)
    installed = os.path.join(bin_dir, program)
    shutil.copy2(cluau, installed)
    with open(os.path.join(doc_dir, "README.md"), "w") as f:
        f.write("the old README\n")
    return installed, doc_dir


def digest(file):
    with open(file, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


try:
    # An upgrade that goes through.
    installed, doc_dir = install(os.path.join(root, "good"))
    # What an upgrade stopped by Ctrl-C leaves: the next one clears it.
    with open(os.path.join(os.path.dirname(installed), "cluau.new.1.exe" if windows else ".cluau.new.1"), "w") as f:
        f.write("left over")
    before = digest(installed)
    good = make_release("good")
    out = run([installed, "upgrade", "--version", tag], {"CLUAU_DOWNLOAD_URL": good, "CLUAU_RELEASE_KEYS": key.public})
    check("a signed release installs", out.returncode == 0 and f"to {tag}" in out.stdout, out.stdout + out.stderr)
    with open(os.path.join(doc_dir, "README.md")) as f:
        check("the guides are replaced too", f.read() == "the new README\n")
    check("the new command is in place and runs", run([installed, "--version"]).stdout.strip() == f"cluau {version}")
    leftovers = [n for n in os.listdir(os.path.dirname(installed)) if n != program and not n.endswith(".old")]
    check("nothing is left beside the command", leftovers == [], str(leftovers))
    check("the command is the release's", digest(installed) == before)

    # A signature made the way minisign did before 0.11 (of the file itself).
    legacy = make_release("legacy", prehashed=False)
    out = run([installed, "upgrade", "--version", tag], {"CLUAU_DOWNLOAD_URL": legacy, "CLUAU_RELEASE_KEYS": f"{other.public} {key.public}"})
    check("a legacy signature, among two keys", out.returncode == 0, out.stdout + out.stderr)

    # Releases that are refused, each leaving the command as it was.
    # (Refused before the command is unpacked: a small one stands in.)
    small = b"refused before it is unpacked"
    refused = [
        ("a file changed after signing", make_release("changed", command=small, tamper_sums=True), key, "does not match"),
        ("a key it does not trust", make_release("untrusted", command=small, sign_with=other), key, "not one this cluau trusts"),
        ("a trusted comment changed", make_release("comment", command=small, tamper_comment=True), key, "trusted comment does not match"),
        ("a signature for another release", make_release("other-release", command=small, comment="cluau v0.0.1"), key, 'signed as "cluau v0.0.1"'),
        ("an archive that does not match", make_release("archive", command=small, tamper_archive=True), key, "does not match its checksum"),
        ("a release with no signature", make_release("unsigned", command=small, signed=False), key, "is not signed"),
        ("a command that does not run", make_release("broken", command=b"#!/bin/sh\necho broken >&2\nexit 3\n" if not windows else b"not a program"), key, "does not run on this system"),
    ]
    for name, directory, trusted, expected in refused:
        target = os.path.join(root, "refused-" + name.replace(" ", "-"))
        installed, _ = install(target)
        before = digest(installed)
        out = run([installed, "upgrade", "--version", tag], {"CLUAU_DOWNLOAD_URL": directory, "CLUAU_RELEASE_KEYS": trusted.public})
        check(f"refused: {name}", out.returncode == 1 and expected in out.stderr, out.stdout + out.stderr)
        check(f"refused: {name}: the command is unchanged", digest(installed) == before)
        names = sorted(os.listdir(os.path.dirname(installed)))
        check(f"refused: {name}: nothing is left beside the command", names == [program], str(names))

    out = run([cluau, "upgrade", "--version", tag], {"CLUAU_DOWNLOAD_URL": good, "CLUAU_RELEASE_KEYS": "not-a-key"})
    check("a key that is not a minisign key", out.returncode == 1 and "not a minisign public key" in out.stderr, out.stderr)
    out = run([cluau, "upgrade", "--check"], {"CLUAU_DOWNLOAD_URL": good})
    check("CLUAU_DOWNLOAD_URL needs a version", out.returncode == 1 and "needs a version" in out.stderr, out.stderr)
    out = run([cluau, "upgrade", "--bogus"])
    check("an unknown option", out.returncode == 2 and "usage: cluau upgrade" in out.stderr, out.stderr)
finally:
    shutil.rmtree(root, ignore_errors=True)

if failures:
    print(f"{len(failures)} failure(s)")
    sys.exit(1)
print("all upgrade tests passed")
