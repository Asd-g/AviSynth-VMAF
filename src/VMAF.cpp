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

#include "avisynth.h"
#include "libvmaf.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

class VMAF : public GenericVideoFilter
{
    PClip distorted_;
    double vmafScore;
    char* fmt, * logfmt, * pool_;
    std::unique_ptr<char[]> modelPath, logPath;
    bool ssim_, ms_ssim_, ci_;
    std::thread* vmafThread;

    void callVMAF() noexcept;

public:
    inline static PVideoFrame reference, main;
    inline static bool frameSet, eof;
    inline static float divisor;
    inline static std::mutex mtx;
    inline static std::condition_variable cond;
    inline static int error;

    VMAF(PClip _child, PClip distorted, int model, const char* log_path, int log_fmt, bool ssim, bool ms_ssim, int pool, bool ci, IScriptEnvironment* env);
    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
    int __stdcall SetCacheHints(int cachehints, int frame_range)
    {
        return cachehints == CACHE_GET_MTMODE ? MT_SERIALIZED : 0;
    }

    ~VMAF();
};

template<typename T>
static int readFrame(float* __restrict refData, float* __restrict mainData, float* __restrict tempData, const int strideByte, void* userData) noexcept
{
    std::unique_lock<std::mutex> lck{ VMAF::mtx };
    while (!VMAF::frameSet && !VMAF::eof)
        VMAF::cond.wait(lck);

    if (VMAF::frameSet)
    {
        const int pixel_size = sizeof(T);
        const int srcStride = VMAF::reference->GetPitch() / pixel_size;
        const int dstStride = strideByte / sizeof(float);
        const int height = VMAF::reference->GetHeight();
        const int width = VMAF::reference->GetRowSize() / pixel_size;
        const T* refp = reinterpret_cast<const T*>(VMAF::reference->GetReadPtr());
        const T* mainp = reinterpret_cast<const T*>(VMAF::main->GetReadPtr());

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
                    refData[x] = refp[x] * VMAF::divisor;
                    mainData[x] = mainp[x] * VMAF::divisor;
                }
            }

            refp += srcStride;
            mainp += srcStride;
            refData += dstStride;
            mainData += dstStride;
        }
    }

    const bool ret = !VMAF::frameSet;

    VMAF::reference = nullptr;
    VMAF::main = nullptr;
    VMAF::frameSet = false;

    VMAF::cond.notify_one();

    return ret ? 2 : 0;
}

void VMAF::callVMAF() noexcept
{
    if (vi.ComponentSize() == 1)
        VMAF::error = compute_vmaf(&vmafScore, fmt, vi.width, vi.height, readFrame<uint8_t>, 0, modelPath.get(), logPath.get(), logfmt, 0, 0, 0, 0, 0, ssim_, ms_ssim_, pool_, 0, 1, ci_);
    else
        VMAF::error = compute_vmaf(&vmafScore, fmt, vi.width, vi.height, readFrame<uint16_t>, 0, modelPath.get(), logPath.get(), logfmt, 0, 0, 0, 0, 0, ssim_, ms_ssim_, pool_, 0, 1, ci_);

    if (VMAF::error)
    {
        VMAF::mtx.lock();
        VMAF::cond.notify_one();
        VMAF::mtx.unlock();
    }
}

VMAF::VMAF(PClip _child, PClip distorted, int model, const char* log_path, int log_fmt, bool ssim, bool ms_ssim, int pool, bool ci, IScriptEnvironment* env)
    : GenericVideoFilter(_child), distorted_(distorted), ssim_(ssim), ms_ssim_(ms_ssim), ci_(ci)
{
    if (vi.BitsPerComponent() > 16)
        env->ThrowError("VMAF: only 8..16-bit input supported.");
    if (vi.IsRGB())
        env->ThrowError("VMAF: RGB color family is not supported.");
    if (!vi.IsPlanar())
        env->ThrowError("VMAF: only planar format supported.");

    const VideoInfo& vi1 = distorted->GetVideoInfo();
    if (!vi.IsSameColorspace(vi1))
        env->ThrowError("VMAF: both clips must be the same format.");
    if (vi.width != vi1.width || vi.height != vi1.height)
        env->ThrowError("VMAF: both clips must have the same dimensions.");
    if (vi.num_frames != vi1.num_frames)
        env->ThrowError("VMAF: both clips' number of frames don't match.");

    if (model < 0 || model > 1)
        env->ThrowError("VMAF: model must be 0 or 1.");
    if (log_fmt < 0 || log_fmt > 2)
        env->ThrowError("VMAF: log_fmt must be 0, 1, or 2.");
    if (pool < 0 || pool > 2)
        env->ThrowError("VMAF: pool must be 0, 1, or 2.");

    fmt = const_cast<char*>("yuv420p");

    std::vector<wchar_t> charBuffer;
    do
    {
        charBuffer.resize(512);
    } while (GetModuleFileNameW((HINSTANCE)&__ImageBase, charBuffer.data(), 512) == 512);
    std::filesystem::path pluginPath(charBuffer.data());
    std::string path = pluginPath.remove_filename().string();

    if (model == 0)
        path += ci_ ? "model\\vmaf_b_v0.6.3\\vmaf_b_v0.6.3.pkl" : "model\\vmaf_v0.6.1.pkl";
    else
        path += ci_ ? "model\\vmaf_4k_rb_v0.6.2\\vmaf_4k_rb_v0.6.2.pkl" : "model\\vmaf_4k_v0.6.1.pkl";

    modelPath = std::make_unique<char[]>(path.length() + 1);
    strcpy_s(modelPath.get(), path.size() + 1, path.c_str());

    if (log_path)
    {
        logPath = std::make_unique<char[]>(*log_path);
        strcpy_s(logPath.get(), strlen(log_path) + 1, log_path);
    }

    switch (log_fmt)
    {
        case 0: logfmt = const_cast<char*>("xml"); break;
        case 1: logfmt = const_cast<char*>("json"); break;
        default: logfmt = const_cast<char*>("csv"); break;
    }

    switch (pool)
    {
        case 0: pool_ = const_cast<char*>("mean"); break;
        case 1: pool_ = const_cast<char*>("harmonic_mean"); break;
        default: pool_ = const_cast<char*>("min"); break;
    }

    divisor = 1.0f / (1 << (vi.BitsPerComponent() - 8));

    vmafThread = new std::thread(&VMAF::callVMAF, this);
}

VMAF::~VMAF()
{
    mtx.lock();
    eof = true;
    cond.notify_one();
    mtx.unlock();

    vmafThread->join();

    delete vmafThread;
}

PVideoFrame __stdcall VMAF::GetFrame(int n, IScriptEnvironment* env)
{
    std::unique_lock<std::mutex> lck{ mtx };
    while (frameSet && !VMAF::error)
        cond.wait(lck);

    PVideoFrame ref = child->GetFrame(n, env);
    PVideoFrame dist = distorted_->GetFrame(n, env);
    reference = ref;
    main = dist;
    frameSet = true;

    cond.notify_one();

    return ref;
}

AVSValue __cdecl Create_VMAF(AVSValue args, void* user_data, IScriptEnvironment* env)
{
    return new VMAF(
        args[0].AsClip(),
        args[1].AsClip(),
        args[2].AsInt(0),
        args[3].AsString(),
        args[4].AsInt(0),
        args[5].AsBool(false),
        args[6].AsBool(false),
        args[7].AsInt(1),
        args[8].AsBool(false),
        env);
}

const AVS_Linkage* AVS_linkage;

extern "C" __declspec(dllexport)
const char* __stdcall AvisynthPluginInit3(IScriptEnvironment * env, const AVS_Linkage* const vectors)
{
    AVS_linkage = vectors;

    env->AddFunction("VMAF", "cc[model]i[log_path]s[log_fmt]i[ssim]b[ms_ssim]b[pool]i[ci]b", Create_VMAF, 0);
    return "VMAF";
}
