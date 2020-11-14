# Description

VMAF is a perceptual video quality assessment algorithm developed by Netflix. Refer to the [FAQ](https://github.com/Netflix/vmaf/blob/master/FAQ.md) page for frequently asked questions of VMAF.

This is [a port of the VapourSynth plugin VMAF](https://github.com/HomeOfVapourSynthEvolution/VapourSynth-VMAF).

[vmaf](https://github.com/Netflix/vmaf/tree/v1.3.15) is used.

# Usage

```
VMAF (clip reference, clip distorted, int "model", string "log_path", int "log_fmt", bool "ssim", bool "ms_ssim", int "pool", bool "ci")
```

## Parameters:

- reference, distorted\
    Clips to calculate VMAF score.\
    Must be in YUV 8..16-bit planar format.
    
- model\
    Sets which model to use. Refer to the [models](https://github.com/Netflix/vmaf/blob/v1.3.15/resource/doc/models.md) page for more details.\
    0: vmaf_v0.6.1.pkl\
    1: vmaf_4k_v0.6.1.pkl\
    Default: 0.

- log_path\
    Sets the path of the log file.\
    Default: None.
    
- log_fmt\
    Sets the format of the log file.\
    0: xml.\
    1: json.\
    2: csv.\
    Default: 0.
    
- ssim\
    Whether to also calculate SSIM score.\
    Default: False.
    
- ms_ssim\
    Whether to also calculate MS-SSIM score.\
    Default: False.
    
- pool\
    Sets the method to pool the per-frame scores.\
    0: Mean.\
    1: Harmonic mean.\
    2: Min.\
    Default: 1.
    
- ci\
    Whether to enable confidence interval.\
    True: It uses vmaf_b_v0.6.3 for `model=0` and vmaf_4k_rb_v0.6.2 for `model=1`.\
    Refer to the [VMAF confidence interval page](https://github.com/Netflix/vmaf/blob/v1.3.15/resource/doc/conf_interval.md) for more details.\
    Default: False.
    