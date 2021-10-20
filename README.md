## Description

VMAF is a perceptual video quality assessment algorithm developed by Netflix. Refer to the [FAQ](https://github.com/Netflix/vmaf/blob/master/FAQ.md) page for frequently asked questions of VMAF.

This is [a port of the VapourSynth plugin VMAF](https://github.com/HomeOfVapourSynthEvolution/VapourSynth-VMAF).

[vmaf](https://github.com/Netflix/vmaf) is used.

### Requirements:

- AviSynth 2.60 / AviSynth+ 3.4 or later

### Usage:

```
VMAF (clip reference, clip distorted, string log_path, int "log_format", int[] "model", int[] "feature")
```

### Parameters:

- reference, distorted\
    Clips to calculate VMAF score.\
    Must be in YUV 8..16-bit planar format with minimum three planes.

- log_path\
    Sets the path of the log file.
    
- log_format\
    Sets the format of the log file.\
    0: xml\
    1: json\
    2: csv\
    Default: 0.
    
- model\
    Sets which model to use.\
    Refer to [this](https://github.com/Netflix/vmaf/blob/master/resource/doc/models.md), [this](https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30) and [this](https://github.com/Netflix/vmaf/blob/master/resource/doc/conf_interval.md) for more details.\
    0: vmaf_v0.6.1\
    1: vmaf_v0.6.1neg (NEG mode)\
    2: vmaf_b_v0.6.3 (Confidence Interval)\
    3: vmaf_4k_v0.6.1\
    Default: 0.

- feature\
    0: PSNR\
    1: PSNR-HVS\
    2: SSIM\
    3: MS-SSIM\
    4: CIEDE2000\
    5: cambi
    
### Building:

```
Requirements:
    - Git
    - GCC C++17 compiler
    - CMake >= 3.16
    - AviSynth library
    - meson
```
```
git clone --recurse-submodules https://github.com/Asd-g/AviSynth-VMAF && \
cd AviSynth-VMAF\vmaf && \
mkdir vmaf_install && \
meson setup libvmaf libvmaf/build --buildtype release --default-library static --prefix $(pwd)/vmaf_install && \
meson install -C libvmaf/build && \
cd .. && \
cmake -B build .
cmake --build build
```
