#!/usr/bin/env python3
"""Build the native VPK with pinned softfp dependencies (Python 3.11+)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = "sdk-vita_softfp-snapshot-20260828.667.1"
PACKAGES = "packages-snapshot-20261015.54.1"
SDK_HASHES = {
    ("Darwin", "arm64"): ("arm64-apple-darwin", "2a01a51390f99846f058d5d29525ba65f4992a722a793a0adb78183f0ff2a9bd"),
    ("Linux", "x86_64"): ("x86_64-linux-gnu", "d6ccefa998607d5ad1599bfb07e97f19be18c14cad7dca71441861aa1a6d0107"),
}
PACKAGE_HASHES = {
    'sdl2_image-2.8.12-1-vita_softfp.pkg.tar.xz': '06fc17731c3c3f5034d37e92847af45e428b4579d534303726bbb5ef1f50d890',
    'freetype-2.14.3-1-vita_softfp.pkg.tar.xz': 'fd6be5953848eb72b99350e83d19bfa77f0d88dbedaef51e4fc6437fbd44418c',
    'harfbuzz-14.5.0-1-vita_softfp.pkg.tar.xz': '7eaf4d9d9dc2537cd808a628862e8ce6952c8ec5571f72ba2c314fae43421330',
    'libass-0.17.5-1-vita_softfp.pkg.tar.xz': 'b5a0569b7e9c1227cf9aa1ffe290c04fa82eb64ea767b184335452bd336a9d21',
    'fribidi-1.0.17-1-vita_softfp.pkg.tar.xz': 'e36b8ac50bdfed7b5aeb2c7691060e25c89513f37681e68042e70ab62cbfbf2d',
    'luajit-2.1.0-1-vita_softfp.pkg.tar.xz': 'ead512f1a17f9baff841fd2c860fafb7a34eb0660214d7dd1f020d51f196ec53',
    'libogg-1.3.6-2-vita_softfp.pkg.tar.xz': 'f58f91dfda58c5a9ba41009c902eaada653f07984b44844ddeba6890fa3b6b55',
    'libvorbis-1.3.7-2-vita_softfp.pkg.tar.xz': 'e0cd0a2022552fa636cbecf7eccef2c0acd8360f52646cf4326b612ebb5d4b88',
    'opus-1.6.1-1-vita_softfp.pkg.tar.xz': 'f6ef09d1108033e1cd37e5da5786bc57a1fb6daf6c0d639c83c6b0fd1e46f3a3',
    'opusfile-0.12-1-vita_softfp.pkg.tar.xz': 'db6d4209b40ec13ea0a32cc459116d69740e9a6ed474e3d59cb77a565dec4a48',
    'mpg123-1.33.7-1-vita_softfp.pkg.tar.xz': '668e9660def53c56439849af41a3360a904fa20e720bc76808df3936e6fcd29b',
    'libmodplug-0.8.9.0.r461.gd1b97ed-1-vita_softfp.pkg.tar.xz': '36b2a774bac5e23643701e2635049b26526b92a8e1850922c1d523c24aa49daa',
    'libxmp-lite-4.7.3-1-vita_softfp.pkg.tar.xz': '5286ba6083f0f02fd5b3bf043daf73f4fb353fa9c17576eca7353aaa42d710cd',
    'libjpeg-turbo-3.2.0-1-vita_softfp.pkg.tar.xz': '564ca12a8ba225c53cdee8495dc079301a8c1ccba89d1fee0f08afcf81a2c24b',
    'libwebp-1.6.0-1-vita_softfp.pkg.tar.xz': '27206fed5b1d3440cf12b37fc8a9611f46279ed12e788955e4c4ba04cb3d2126',
    'vita-libdl-0.0.0.r2.gbad0b43-1-vita_softfp.pkg.tar.xz': '4f21c496930a9fca6b9d7155e094f876470c8abf2f1124cd31ad8ecef103ee60',
    'vitaShaRK-1.7-1-vita_softfp.pkg.tar.xz': '26cf8939e8bcc9955a4c3645b880458e67f17009ada00d6b518df7268732a803',
    'libmathneon-0.0.0.r11.g0faab81-1-vita_softfp.pkg.tar.xz': '62b743201c276a152cef7f3ecce4969a094911efdebe037729d6e92af27febb7',
    'taihen-0.11-1-vita_softfp.pkg.tar.xz': 'de07e819407dd5927ad2a24d3116d1278203dc0f8beef368bb2626adf4c55e0d',
    'SceShaccCgExt-1.0.1-1-vita_softfp.pkg.tar.xz': '5ab525600b4f306b9d99e91114f73981d3db844158ba73cf30c06138a493b1fa',
    'zlib-1.3.2-2-vita_softfp.pkg.tar.xz': 'a1804fe899e93b50bae8cf0a97fbcde384c5b58e9460e805cb18503100cfa7e6',
    'bzip2-1.0.8-1-vita_softfp.pkg.tar.xz': 'a263bd8c2a94fdcbcd2e9f98e113e2bd31833dd78897f7f491acace2414ebb83',
    'flac-1.3.4-1-vita_softfp.pkg.tar.xz': 'b8f548569faff32d3fd6ed6b4f9e32736a0fbd2a15c8b1d6e1520dddfdd65beb',
}
VITAGL = "c2a950fbf554f5ec5bbb27c4d387a03f70b0eb6c"
SDL2_VITAGL = "f99bb638f7eb256fa812e9da15d4e295a170640c"


def run(*command, cwd=ROOT):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, check=True)


def download(url, sha256):
    path = downloads / url.rsplit("/", 1)[1]
    if not path.exists():
        run("curl", "--fail", "--location", "--retry", "3", "--output", path, url)
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        raise ValueError(f"SHA-256 mismatch: {path}")
    return path


def source(url, sha256):
    archive = download(url, sha256)
    with tarfile.open(archive) as tar:
        top = Path(tar.getmembers()[0].name).parts[0]
        path = deps / top
        if not path.exists():
            tar.extractall(deps, filter="data")
    return path


def cmake_build(source_dir, name, *options):
    build = deps / name
    run("cmake", "-S", source_dir, "-B", build,
        f"-DCMAKE_TOOLCHAIN_FILE={sdk}/share/vita.toolchain.cmake",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_BUILD_TYPE=Release", *options)
    run("cmake", "--build", build, "--parallel", args.jobs)
    run("cmake", "--install", build)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
args = parser.parse_args()
if sys.version_info < (3, 11):
    parser.error("Python 3.11 or newer is required")
deps = ROOT / "build/native-deps"
prefix = deps / "install"
downloads = deps / "downloads"
downloads.mkdir(parents=True, exist_ok=True)
sdk = Path(os.environ.get("VITASDK", str(ROOT / "build/vitasdk"))).resolve()
if not (sdk / "bin/arm-vita-eabi-gcc").exists():
    if sdk.exists():
        raise ValueError(f"SDK directory exists without a compiler: {sdk}")
    host, digest = SDK_HASHES[(platform.system(), platform.machine())]
    archive = download(f"https://github.com/vitasdk/autobuilds/releases/download/{CORE}/vitasdk-bootstrap-{host}.tar.bz2", digest)
    with tempfile.TemporaryDirectory(dir=deps) as temporary:
        with tarfile.open(archive) as tar:
            tar.extractall(temporary, filter="data")
        sdk.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(Path(temporary) / "vitasdk"), sdk)
os.environ["VITASDK"] = str(sdk)
os.environ["PATH"] = str(sdk / "bin") + os.pathsep + os.environ["PATH"]
run("arm-vita-eabi-gcc", "--version")


def install_package(item):
    name, digest = item
    archive = download(f"https://github.com/vitasdk/vitasdk-autobuild/releases/download/{PACKAGES}/{name}", digest)
    with tarfile.open(archive) as tar:
        members = [member for member in tar.getmembers() if member.name.startswith("arm-vita-eabi/")]
        tar.extractall(sdk, members=members, filter="data")


with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(install_package, PACKAGE_HASHES.items()))
(prefix / "lib").mkdir(parents=True, exist_ok=True)
(prefix / "include").mkdir(exist_ok=True)

# This revision matches both vendored translator reference files and the
# framebuffer prefix / matrix-table binary format consumed by the port.
driver = source(f"https://github.com/Rinnegatamante/vitaGL/archive/{VITAGL}.tar.gz",
                "e7c195d5a3c5beb7ec51e0a629c4d7366dbfdd06b33c3c1592c604161f882208")
for name in ("glsl_utils.c", "glsl_utils.h"):
    if (driver / "source/utils" / name).read_bytes() != (ROOT / "vendor/vitagl-translator/upstream" / name).read_bytes():
        raise ValueError(f"vitaGL source does not match the port: {name}")
# The pinned SDK already bridges softfp GXM calls. vitaGL's legacy
# SOFTFP_ABI option would marshal viewport arguments a second time.
run("make", "clean", cwd=driver)
run("make", f"-j{args.jobs}", "NO_DEBUG=1", "HAVE_GLSL_SUPPORT=1", cwd=driver)
# Debug paths are host-specific and must not change the shader fingerprint.
run("arm-vita-eabi-objcopy", "--enable-deterministic-archives", "--strip-debug", "--preserve-dates",
    driver / "libvitaGL.a", prefix / "lib/libvitaGL.a")
shutil.copy2(driver / "source/vitaGL.h", prefix / "include/vitaGL.h")

sdl = source(f"https://github.com/Northfear/SDL/archive/{SDL2_VITAGL}.tar.gz",
             "e0392c4f7d0b8aa06f0182564d624ab980e171a8fcba532a37874405b9c5f977")
cmake_build(sdl, "sdl-build", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST=OFF",
            "-DVIDEO_VITA_VGL=ON", "-DVIDEO_VITA_PIB=OFF", "-DVIDEO_VITA_PVR=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=OFF",
            f"-DCMAKE_C_FLAGS=-mfloat-abi=softfp -I{prefix}/include -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types")
symbols = subprocess.check_output(["arm-vita-eabi-nm", "-g", str(prefix / "lib/libSDL2.a")],
                                  text=True, stderr=subprocess.DEVNULL)
for binding, symbol in (("T", "VITA_GLES_CreateContext"), ("T", "VITA_GLES_SwapWindow"),
                        ("U", "vglInitExtended")):
    if f" {binding} {symbol}\n" not in symbols:
        raise ValueError(f"SDL2 is missing the required vitaGL backend: {symbol}")

mixer = source("https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.2/SDL2_mixer-2.8.2.tar.gz",
               "938dff531d00ace2296557a6599abe6f34599e2f34f0a4a08a397e2ccac8b8f7")
path = mixer / "src/mixer.c"
text = path.read_text()
start = text.index("static SDL_AudioSpec *Mix_LoadMusic_RW(")
end = text.index("\nMix_Chunk *Mix_LoadWAV_RW(", start)
decoder = text[start:end]
for call in ("    Mix_LockAudio();\n", "    Mix_UnlockAudio();\n"):
    if decoder.count(call) > 1:
        raise ValueError("SDL_mixer decoder no longer matches the upstream lock patch")
    decoder = decoder.replace(call, "")
patched = text[:start] + decoder + text[end:]
if patched != text:
    path.write_text(patched)
cmake_build(mixer, "mixer-build", "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=OFF",
            "-DSDL2MIXER_VENDORED=OFF",
            "-DSDL2MIXER_DEPS_SHARED=OFF", "-DSDL2MIXER_SAMPLES=OFF", "-DSDL2MIXER_CMD=OFF",
            "-DSDL2MIXER_MIDI=OFF", "-DSDL2MIXER_FLAC=OFF", "-DSDL2MIXER_WAVPACK=OFF",
            "-DSDL2MIXER_MOD_XMP=ON", "-DSDL2MIXER_MOD_XMP_LITE=ON", "-DSDL2MIXER_VORBIS=VORBISFILE",
            f"-DSDL2_DIR={prefix}/lib/cmake/SDL2",
            "-DCMAKE_C_FLAGS=-mfloat-abi=softfp -Wno-error=incompatible-pointer-types")
png = source("https://downloads.sourceforge.net/libpng/libpng-1.6.58.tar.xz",
             "28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775")
cmake_build(png, "png-build", "-DPNG_ARM_NEON=off", "-DPNG_SHARED=OFF", "-DPNG_TESTS=OFF",
            "-DPNG_TOOLS=OFF", "-DCMAKE_C_FLAGS=-mfloat-abi=softfp")

ffmpeg = source("https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz",
                "57be87c22d9b49c112b6d24bc67d42508660e6b718b3db89c44e47e289137082")
ffbuild = deps / "ffmpeg-build"
ffbuild.mkdir(exist_ok=True)
if not (ffbuild / "Makefile").exists():
    run(ffmpeg / "configure", f"--prefix={prefix}", "--enable-cross-compile",
        "--cross-prefix=arm-vita-eabi-", "--arch=armv7-a", "--cpu=cortex-a9", "--target-os=none",
        "--disable-shared", "--enable-static", "--disable-programs", "--disable-doc", "--disable-network",
        "--disable-everything", "--disable-avdevice", "--disable-avfilter", "--disable-runtime-cpudetect",
        "--disable-armv5te", "--disable-armv6t2", "--enable-small", "--disable-debug", "--disable-bzlib",
        "--disable-iconv", "--disable-lzma", "--disable-sdl2", "--disable-securetransport", "--disable-xlib",
        "--enable-pthreads", "--enable-protocol=file", "--enable-swscale", "--enable-swresample",
        "--enable-decoder=mpeg2video,h264,mjpeg,png,bmp,aac,mp3,vorbis,opus,flac,pcm_s16le,pcm_s16be,pcm_u8",
        "--enable-demuxer=mpegvideo,mov,ogg,matroska,wav,mp3,flac,image2",
        "--enable-parser=mpegvideo,h264,aac,ac3,mpegaudio,vorbis,opus",
        "--extra-cflags=-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -D_BSD_SOURCE -Wno-error=incompatible-pointer-types",
        cwd=ffbuild)
run("make", f"-j{args.jobs}", cwd=ffbuild)
components = ("avformat", "avcodec", "swresample", "swscale", "avutil")
if any(not (prefix / f"lib/lib{name}.a").exists() or
       (prefix / f"lib/lib{name}.a").read_bytes() != (ffbuild / f"lib{name}/lib{name}.a").read_bytes()
       for name in components):
    run("make", "install", cwd=ffbuild)

build = ROOT / "native-build"
run("cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
    f"-DONS_NATIVE_DEPS_ROOT={prefix}", f"-DONS_FFMPEG_ROOT={prefix}", f"-DPython3_EXECUTABLE={sys.executable}")
run("cmake", "--build", build, "--parallel", args.jobs)
print(f"VPK: {build / 'umineko-native.vpk'}")
