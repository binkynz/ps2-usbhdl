# Default: list available recipes.
default:
    @just --list

# Build the installer ELF into dist/. Forwards extra args to make,
# e.g. `just build clean` to clean inside the container.
build *ARGS:
    @docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        make {{ARGS}}

# Build a tiny test ISO for fast install-pipeline iteration. Output:
# dist/test.iso (~820 KB; streams over USB 1.1 in under a second
# vs. ~67 minutes for a real DVD ISO).
test-iso:
    @docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    @mkdir -p dist
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        sh /src/test/build-iso.sh
    @ls -la dist/test.iso

# Build both the installer ELF and the test ISO.
all: build test-iso

# Copy build outputs + INSTALL_NOW sentinel onto a mounted USB stick.
# Usage: `just deploy /run/media/$USER/<stick>`
deploy DIR:
    @test -d "{{DIR}}" || { echo "not a directory: {{DIR}}" >&2; exit 1; }
    cp -v dist/ps2-usbhdl.elf "{{DIR}}/"
    @[ -f dist/test.iso ] && cp -v dist/test.iso "{{DIR}}/" || true
    touch "{{DIR}}/INSTALL_NOW"
    sync
    @echo "ready - unmount the stick and plug into the PS2."

# Remove host + container build artifacts.
clean:
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        make clean
    rm -rf dist
