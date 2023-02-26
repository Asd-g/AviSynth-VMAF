#pragma once

/*
  MIT License

  Copyright (c) 2018-2019 HolyWu
  Copyright (c) 2020-2022 Asd-g

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <regex>
#include <vector>

#include "avisynth_c.h"

extern "C" {
#include "libvmaf/libvmaf.h"
}

using namespace std::literals;

static constexpr const char *modelName[] = {"vmaf", "vmaf_neg", "vmaf_b", "vmaf_4k"};
static constexpr const char *modelVersion[] = {"vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_b_v0.6.3", "vmaf_4k_v0.6.1"};
static constexpr const char *featureName[] = {"psnr", "psnr_hvs", "float_ssim", "float_ms_ssim", "ciede", "cambi"};

AVS_Value AVSC_CC Create_VMAF(AVS_ScriptEnvironment *env, AVS_Value args, void *param);
AVS_Value AVSC_CC Create_VMAF2(AVS_ScriptEnvironment *env, AVS_Value args, void *param);
