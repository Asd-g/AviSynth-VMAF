/*
  MIT License
  Copyright (c) 2018-2019 HolyWu
  Copyright (c) 2020 Asd-g
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

#include <Windows.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "avisynth_c.h"
#include "libvmaf.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

struct VMAF
{
    AVS_Clip* distorted;
    const AVS_VideoInfo* vi;
    double vmafScore;
    char* fmt, * logfmt, * pool;
    std::unique_ptr<char[]> modelPath, logPath;
    bool ssim, ms_ssim, ci;
    std::thread* vmafThread;

    AVS_VideoFrame* reference,* main;
    bool frameSet, eof;
    float divisor;
    std::mutex mtx;
    std::condition_variable cond;
    int error;
};

template<typename T>
int readFrame(float* __restrict refData, float* __restrict mainData, float* __restrict tempData, const int strideByte, void* userData) noexcept
{
    VMAF* const __restrict d = static_cast<VMAF*>(userData);

    std::unique_lock<std::mutex> lck{ d->mtx };
    while (!d->frameSet && !d->eof)
        d->cond.wait(lck);

    if (d->frameSet)
    {
        const int pixel_size = sizeof(T);
        const int srcStride = avs_get_pitch(d->reference) / pixel_size;
        const int dstStride = strideByte / sizeof(float);
        const int height = avs_get_height(d->reference);
        const int width = avs_get_row_size(d->reference) / pixel_size;
        const T* refp = reinterpret_cast<const T*>(avs_get_read_ptr(d->reference));
        const T* mainp = reinterpret_cast<const T*>(avs_get_read_ptr(d->main));

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if constexpr (std::is_same_v<T, uint8_t>)
                {
                    refData[x] = refp[x];
                    mainData[x] = mainp[x];
                }
                else
                {
                    refData[x] = refp[x] * d->divisor;
                    mainData[x] = mainp[x] * d->divisor;
                }
            }

            refp += srcStride;
            mainp += srcStride;
            refData += dstStride;
            mainData += dstStride;
        }
    }

    const bool ret = !d->frameSet;

    d->reference = nullptr;
    d->main = nullptr;
    d->frameSet = false;

    d->cond.notify_one();

    return ret ? 2 : 0;
}

void callVMAF(VMAF* const __restrict d) noexcept
{
    if (avs_component_size(d->vi) == 1)
        d->error = compute_vmaf(&d->vmafScore, d->fmt, d->vi->width, d->vi->height, readFrame<uint8_t>, d, d->modelPath.get(), d->logPath.get(), d->logfmt, 0, 0, 0, 0, 0, d->ssim, d->ms_ssim, d->pool, 0, 1, d->ci);
    else
        d->error = compute_vmaf(&d->vmafScore, d->fmt, d->vi->width, d->vi->height, readFrame<uint16_t>, d, d->modelPath.get(), d->logPath.get(), d->logfmt, 0, 0, 0, 0, 0, d->ssim, d->ms_ssim, d->pool, 0, 1, d->ci);

    if (d->error)
    {
        d->mtx.lock();
        d->cond.notify_one();
        d->mtx.unlock();
    }
}

AVS_VideoFrame* AVSC_CC vmaf_get_frame(AVS_FilterInfo* fi, int n)
{
    VMAF* d = reinterpret_cast<VMAF*>(fi->user_data);

    std::unique_lock<std::mutex> lck{ d->mtx };
    while (d->frameSet && !d->error)
        d->cond.wait(lck);

    AVS_VideoFrame* ref = avs_get_frame(fi->child, n);
    AVS_VideoFrame* dist = avs_get_frame(d->distorted, n);
    d->reference = ref;
    d->main = dist;
    d->frameSet = true;

    d->cond.notify_one();

    avs_release_video_frame(dist);
    return ref;
}

void AVSC_CC free_vmaf(AVS_FilterInfo* fi)
{
    VMAF* d = reinterpret_cast<VMAF*>(fi->user_data);

    d->mtx.lock();
    d->eof = true;
    d->cond.notify_one();
    d->mtx.unlock();

    d->vmafThread->join();

    delete d->vmafThread;

    avs_release_clip(d->distorted);
    delete d;
}

AVS_Value AVSC_CC Create_VMAF(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    AVS_FilterInfo* fi;

    AVS_Clip* clip = avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1);

    VMAF* params = new VMAF();
    params->vi = &fi->vi;

    params->distorted = avs_take_clip(avs_array_elt(args, 1), env);
    const int model = (avs_defined(avs_array_elt(args, 2))) ? avs_as_int(avs_array_elt(args, 2)) : 0;
    const char* log_path = avs_as_string(avs_array_elt(args, 3));
    const int log_fmt = (avs_defined(avs_array_elt(args, 4))) ? avs_as_int(avs_array_elt(args, 4)) : 0;
    params->ssim = (avs_defined(avs_array_elt(args, 5))) ? avs_as_bool(avs_array_elt(args, 5)) : false;
    params->ms_ssim = (avs_defined(avs_array_elt(args, 6))) ? avs_as_bool(avs_array_elt(args, 6)) : false;
    const int pool = (avs_defined(avs_array_elt(args, 7))) ? avs_as_int(avs_array_elt(args, 7)) : 1;
    params->ci = (avs_defined(avs_array_elt(args, 8))) ? avs_as_bool(avs_array_elt(args, 8)) : false;

    AVS_Value v = avs_void;
    const int bits = avs_bits_per_component(&fi->vi);

    if (!avs_defined(v) && bits > 16)
        v = avs_new_value_error("VMAF: only 8..16-bit input supported.");
    if (!avs_defined(v) && avs_is_rgb(&fi->vi))
        v = avs_new_value_error("VMAF: RGB color family is not supported.");
    if (!avs_defined(v) && !avs_is_planar(&fi->vi))
        v = avs_new_value_error("VMAF: only planar format supported.");

    const AVS_VideoInfo* vi1 = avs_get_video_info(params->distorted);

    if (!avs_defined(v) && !avs_is_same_colorspace(&fi->vi, vi1))
        v = avs_new_value_error("VMAF: both clips must be the same format.");
    if (!avs_defined(v) && fi->vi.width != vi1->width || fi->vi.height != vi1->height)
        v = avs_new_value_error("VMAF: both clips must have the same dimensions.");
    if (!avs_defined(v) && fi->vi.num_frames != vi1->num_frames)
        v = avs_new_value_error("VMAF: both clips' number of frames don't match.");

    if (!avs_defined(v) && (model < 0 || model > 1))
        v = avs_new_value_error("VMAF: model must be 0 or 1.");
    if (!avs_defined(v) && (log_fmt < 0 || log_fmt > 2))
        v = avs_new_value_error("VMAF: log_fmt must be 0, 1, or 2.");
    if (!avs_defined(v) && (pool < 0 || pool > 2))
        v = avs_new_value_error("VMAF: pool must be 0, 1, or 2.");

    params->fmt = const_cast<char*>("yuv420p");

    std::vector<wchar_t> charBuffer;
    charBuffer.reserve(512);
    do {
        charBuffer.resize(512);
    } while (GetModuleFileNameW((HINSTANCE)&__ImageBase, charBuffer.data(), 512) == 512);
    std::filesystem::path pluginPath(charBuffer.data());
    std::string path = pluginPath.remove_filename().string();

    if (model == 0)
        path += params->ci ? "model\\vmaf_b_v0.6.3\\vmaf_b_v0.6.3.pkl" : "model\\vmaf_v0.6.1.pkl";
    else
        path += params->ci ? "model\\vmaf_4k_rb_v0.6.2\\vmaf_4k_rb_v0.6.2.pkl" : "model\\vmaf_4k_v0.6.1.pkl";

    params->modelPath = std::make_unique<char[]>(path.length() + 1);
    strcpy_s(params->modelPath.get(), path.size() + 1, path.c_str());

    if (log_path)
    {
        params->logPath = std::make_unique<char[]>(*log_path);
        strcpy_s(params->logPath.get(), strlen(log_path) + 1, log_path);
    }

    switch (log_fmt)
    {
        case 0: params->logfmt = const_cast<char*>("xml"); break;
        case 1: params->logfmt = const_cast<char*>("json"); break;
        default: params->logfmt = const_cast<char*>("csv"); break;
    }

    switch (pool)
    {
        case 0: params->pool = const_cast<char*>("mean"); break;
        case 1: params->pool = const_cast<char*>("harmonic_mean"); break;
        default: params->pool = const_cast<char*>("min"); break;
    }

    params->divisor = 1.0f / (1 << (bits - 8));

    params->vmafThread = new std::thread(callVMAF, &*params);

    if (!avs_defined(v))
        v = avs_new_value_clip(clip);

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = vmaf_get_frame;
    fi->free_filter = free_vmaf;

    avs_release_clip(clip);

    return v;
}

const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    avs_add_function(env, "VMAF", "cc[model]i[log_path]s[log_fmt]i[ssim]b[ms_ssim]b[pool]i[ci]b", Create_VMAF, 0);
    return "VMAF";
}
