# List available recipes.
default:
    @just --list

# Compile installer ELF -> dist/ps2-usbhdl.elf (forwards args to make).
build *ARGS:
    @docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        make {{ARGS}}

# Build a tiny dist/test.iso (~820 KB) for fast install-pipeline iteration.
test-iso:
    @docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    @mkdir -p dist
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        sh /src/test/build-iso.sh
    @ls -la dist/test.iso

# Build a synthetic ~5 GB dist/test-large.iso to exercise the
# sub-partition path (main 4G + 1 sub) without needing a DVD9 game.
test-iso-large:
    @docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    @mkdir -p dist
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        sh /src/test/build-iso-large.sh
    @ls -la dist/test-large.iso

# Build both the installer ELF and the test ISO.
all: build test-iso

# Stage outputs + INSTALL_NOW onto a USB (block device or mounted dir).
deploy TARGET:
    #!/bin/sh
    # TARGET can be either:
    #   - a block device (/dev/sdX[N]) — sudo-mounts to a tempdir,
    #     copies, syncs, unmounts.
    #   - a directory that's already mounted — copies into it (with
    #     sudo, since system mount points are typically root-owned).
    set -eu

    we_mounted=0
    if [ -b "{{TARGET}}" ]; then
        mp=$(findmnt -nro TARGET "{{TARGET}}" 2>/dev/null || true)
        if [ -z "$mp" ]; then
            mp=$(mktemp -d)
            sudo mount "{{TARGET}}" "$mp"
            we_mounted=1
        fi
    elif [ -d "{{TARGET}}" ]; then
        mp="{{TARGET}}"
    else
        echo "not a block device or directory: {{TARGET}}" >&2
        exit 1
    fi

    echo "Staging to $mp (sudo)..."
    sudo cp -v dist/ps2-usbhdl.elf "$mp/"
    [ -f dist/test.iso ]       && sudo cp -v dist/test.iso       "$mp/" || :
    [ -f dist/test-large.iso ] && sudo cp -v dist/test-large.iso "$mp/" || :
    sync

    if [ "$we_mounted" = "1" ]; then
        sudo umount "{{TARGET}}"
        rmdir "$mp" 2>/dev/null || :
        echo "ready - plug into the PS2."
    else
        echo "ready - unmount the stick and plug into the PS2."
    fi

# List candidate USB block devices for `just deploy`.
usb-list:
    @lsblk -p -o NAME,SIZE,TRAN,MOUNTPOINT,LABEL | awk 'NR==1 || $3=="usb"'

# Format every C source/header in-place via clang-format.
fmt:
    @clang-format -i src/*.c src/*.h test/*.c
    @echo "  formatted."

# Verify every C source/header is clang-format-clean (CI gate).
fmt-check:
    @clang-format --dry-run --Werror src/*.c src/*.h test/*.c

# Install the project's git hooks (clang-format pre-commit check).
install-hooks:
    @mkdir -p .git/hooks
    @ln -sf ../../hooks/pre-commit .git/hooks/pre-commit
    @echo "  pre-commit hook installed (symlinked from hooks/)."

# Extract PS2SDK headers to .ps2sdk/ so clangd / your LSP can resolve
# <kernel.h>, <fileXio_rpc.h>, etc. Run once after cloning. The
# extracted tree is gitignored; re-run if PS2SDK upstream changes.
lsp:
    #!/bin/sh
    set -eu
    docker build -q -t ps2-usbhdl-build:latest . >/dev/null
    rm -rf .ps2sdk
    mkdir -p .ps2sdk/ee .ps2sdk/common
    cid=$(docker create ps2-usbhdl-build:latest)
    trap 'docker rm -f $cid >/dev/null 2>&1 || true' EXIT
    docker cp "$cid:/usr/local/ps2dev/ps2sdk/ee/include"     .ps2sdk/ee/
    docker cp "$cid:/usr/local/ps2dev/ps2sdk/common/include" .ps2sdk/common/
    echo
    echo "  .ps2sdk/ populated. Restart your editor's clangd to pick it up."

# Remove host + container build artifacts.
clean:
    docker run --rm \
        -v "$PWD:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        ps2-usbhdl-build:latest \
        make clean
    rm -rf dist
