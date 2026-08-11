# GarStreamRx native application

This is the canonical RX implementation. Simulation and hardware builds use
the same C++ source and the same Linux device contract:

- GPIO: `/dev/gpiochip*`, character-device API v2
- display: `/dev/spidev*`, ILI9341 RGB565
- stream: RTP/JPEG over UDP
- discovery/control: `gar-stream/1` over UDP

There is no simulation branch or simulation HAL in this code. GAR provides
gpio-sim and the CUSE SPI device below these interfaces on EC2. The Lyra uses
its real kernel devices.

EC2 is aarch64 and Lyra is armv7l, so the executable itself cannot be shared
between them. Build both from this directory: Ubuntu's aarch64 cross compiler
and libraries are used for EC2; the RK3506 Buildroot SDK toolchain/sysroot must
be used for Lyra so its C library and GStreamer ABI match the board image.

The parent GarStreamRx workspace provides `scripts/build-native-rx.sh` for EC2
and `scripts/build-native-rx-target.sh` for RK3506. Direct host tests:

```bash
cmake -S . -B build -DGAR_RX_BUILD_APP=OFF -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Once the RK3506 SDK rootfs has been built, the supported target entry point is:

```bash
cp config/rk3506-sdk.env.example config/rk3506-sdk.env
gar target build --workspace Local/GarStreamRx
```

The parent script mounts the Buildroot `output/.../host` tree into the Docker
build environment and verifies that the result is a 32-bit ARM ELF before it
can enter a target artifact.

The service may override device paths and GPIO offsets through
`/etc/gar/gar-stream-rx.env`; those are deployment data, not compile-time
simulation switches.
