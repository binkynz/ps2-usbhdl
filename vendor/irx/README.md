# Vendored IRX modules

These IOP modules are not part of standard PS2SDK. They come from
[HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller)
by SP193, which is licensed under GPLv2. By embedding them in our
ELF, this project is effectively GPLv2-licensed.

| File              | Source                                    | Purpose                                     |
| ----------------- | ----------------------------------------- | ------------------------------------------- |
| `ps2hdd_hdl.irx`  | HDLGameInstaller `irx/ps2hdd-hdl.irx`     | Drop-in fork of `ps2hdd.irx` with HDL type  |
| `hdlfs.irx`       | HDLGameInstaller `irx/hdlfs.irx`          | Provides `hdl0:` device for HDL formatting  |

The original filename `ps2hdd-hdl.irx` is renamed here with an
underscore (`ps2hdd_hdl.irx`) so the bin2c-generated C symbols stay
valid identifiers (hyphens aren't allowed in C names).

To refresh from upstream:

```sh
git clone --depth 1 https://github.com/ps2homebrew/HDLGameInstaller.git vendor/src/HDLGameInstaller
cp vendor/src/HDLGameInstaller/irx/hdlfs.irx        vendor/irx/hdlfs.irx
cp vendor/src/HDLGameInstaller/irx/ps2hdd-hdl.irx   vendor/irx/ps2hdd_hdl.irx
```
