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

#include <filesystem>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <cstring>
#include <iostream>

#include "avisynth_c.h"

extern "C" {
#include "libvmaf/libvmaf.h"
}

using namespace std::literals;

static constexpr const char* modelName[] = { "vmaf", "vmaf_neg", "vmaf_b", "vmaf_4k" };
static constexpr const char* modelVersion[] = { "vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_b_v0.6.3", "vmaf_4k_v0.6.1" };

static constexpr const char* featureName[] = { "psnr", "psnr_hvs", "float_ssim", "float_ms_ssim", "ciede" };

struct VMAF
{
    AVS_Clip* distorted;
    std::string logPath;
    VmafOutputFormat logFormat;
    std::vector<VmafModel*> model;
    std::vector<VmafModelCollection*> modelCollection;
    VmafContext* vmaf;
    VmafPixelFormat pixelFormat;
    bool chroma;
};

AVS_VideoFrame* AVSC_CC vmaf_get_frame(AVS_FilterInfo* fi, int n)
{
    const char* ErrorText = 0;
    VMAF* d = reinterpret_cast<VMAF*>(fi->user_data);

    AVS_VideoFrame* reference = avs_get_frame(fi->child, n);
    AVS_VideoFrame* distorted = avs_get_frame(d->distorted, n);

    VmafPicture ref{}, dist{};

    if (vmaf_picture_alloc(&ref, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height) ||
        vmaf_picture_alloc(&dist, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height))
        ErrorText = "failed to allocate picture";

    const int pl[3] = { AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    const int planecount = std::min(avs_num_components(&fi->vi), 3);
    for (int plane = 0; plane < planecount; ++plane)
    {
        if (plane && !d->chroma)
            break;

        avs_bit_blt(fi->env, reinterpret_cast<uint8_t*>(ref.data[plane]),
            ref.stride[plane],
            avs_get_read_ptr_p(reference, pl[plane]),
            avs_get_pitch_p(reference, pl[plane]),
            avs_get_row_size_p(reference, pl[plane]),
            avs_get_height_p(reference, pl[plane]));

        avs_bit_blt(fi->env, reinterpret_cast<uint8_t*>(dist.data[plane]),
            dist.stride[plane],
            avs_get_read_ptr_p(distorted, pl[plane]),
            avs_get_pitch_p(distorted, pl[plane]),
            avs_get_row_size_p(distorted, pl[plane]),
            avs_get_height_p(distorted, pl[plane]));
    }

    if (!ErrorText && vmaf_read_pictures(d->vmaf, &ref, &dist, n))
        ErrorText = "failed to read pictures";

    if (ErrorText)
    {
        vmaf_picture_unref(&ref);
        vmaf_picture_unref(&dist);

        avs_release_video_frame(reference);
        avs_release_video_frame(distorted);

        fi->error = ErrorText;

        return 0;
    }
    else
    {
        avs_release_video_frame(distorted);

        return reference;
    }
}

void AVSC_CC free_vmaf(AVS_FilterInfo* fi)
{
    const char* ErrorText = 0;
    VMAF* d = reinterpret_cast<VMAF*>(fi->user_data);

    avs_release_clip(d->distorted);

    if (vmaf_read_pictures(d->vmaf, nullptr, nullptr, 0))
        ErrorText = "failed to flush context";

    if (!ErrorText)
    {
        for (auto&& m : d->model)
            if (double score; vmaf_score_pooled(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, fi->vi.num_frames - 1))
                ErrorText = "failed to generate pooled VMAF score";
    }

    if (!ErrorText)
    {
        for (auto&& m : d->modelCollection)
            if (VmafModelCollectionScore score; vmaf_score_pooled_model_collection(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, fi->vi.num_frames - 1))
                ErrorText = "failed to generate pooled VMAF score";
    }

    if (!ErrorText)
    {
        if (vmaf_write_output(d->vmaf, d->logPath.c_str(), d->logFormat))
            ErrorText = "failed to write VMAF stats";
    }

    for (auto&& m : d->model)
        vmaf_model_destroy(m);
    for (auto&& m : d->modelCollection)
        vmaf_model_collection_destroy(m);
    vmaf_close(d->vmaf);

    delete d;

    if (ErrorText)
        std::cout << ErrorText;
}

AVS_Value AVSC_CC Create_VMAF(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    AVS_FilterInfo* fi;

    AVS_Clip* clip = avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1);

    VMAF* params = new VMAF();

    params->distorted = avs_take_clip(avs_array_elt(args, 1), env);
    params->logPath = avs_as_string(avs_array_elt(args, 2));
    const int logFormat = (avs_is_int(avs_array_elt(args, 3))) ? avs_as_int(avs_array_elt(args, 3)) : 0;

    std::unique_ptr<int[]> model;
    int numModel = [&]() {
        if (avs_defined(avs_array_elt(args, 4)))
        {
            if (avs_is_array(avs_array_elt(args, 4)))
                return avs_array_size(avs_array_elt(args, 4));
            else if (avs_is_int(avs_array_elt(args, 4)))
                return -1;
        }
        else
            return 0;
    }();
    
    if (numModel < 1)
    {
        model = std::make_unique<int[]>(1);
        model[0] = (numModel == 0) ? 0 : avs_as_int(avs_array_elt(args, 4));
        numModel = 1;
    }
    else
    {
        model = std::make_unique<int[]>(numModel);

        for (int i = 0; i < numModel; ++i)
            model[i] = avs_as_int(*(avs_as_array(avs_array_elt(args, 4)) + i));
    }

    params->model.resize(numModel);

    AVS_Value v = avs_void;

    if (avs_bits_per_component(&fi->vi) > 16)
        v = avs_new_value_error("VMAF: only 8..16-bit input supported.");
    if (!avs_defined(v) && avs_is_rgb(&fi->vi))
        v = avs_new_value_error("VMAF: RGB color family is not supported.");
    if (!avs_defined(v) && !avs_is_planar(&fi->vi))
        v = avs_new_value_error("VMAF: only planar format supported.");
    if (!avs_defined(v) && avs_num_components(&fi->vi) < 3)
        v = avs_new_value_error("VMAF: minimum three planes are required.");
    if (!avs_defined(v) && !((avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) == 1 && avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U) == 1) ||
        (avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) == 1 && avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U) == 0) ||
        (avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) && avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U) == 0)))
        v = avs_new_value_error("VMAF: only 420/422/444 chroma subsampling is supported.");

    const AVS_VideoInfo* vi1;

    if (!avs_defined(v))
        vi1 = avs_get_video_info(params->distorted);

    if (!avs_defined(v) && !avs_is_same_colorspace(&fi->vi, vi1))
        v = avs_new_value_error("VMAF: both clips must be the same format.");
    if (!avs_defined(v) && fi->vi.width != vi1->width || fi->vi.height != vi1->height)
        v = avs_new_value_error("VMAF: both clips must have the same dimensions.");
    if (!avs_defined(v) && fi->vi.num_frames != vi1->num_frames)
        v = avs_new_value_error("VMAF: both clips' number of frames don't match.");

    if (!avs_defined(v) && (logFormat < 0 || logFormat > 3))
        v = avs_new_value_error("VMAF: log_fmt must be 0, 1, 2 or 3.");

    if (!avs_defined(v))
    {
        params->logFormat = static_cast<VmafOutputFormat>(logFormat + 1);

        VmafConfiguration configuration{};
        configuration.log_level = VMAF_LOG_LEVEL_INFO;
        configuration.n_threads = std::thread::hardware_concurrency();
        configuration.n_subsample = 1;
        configuration.cpumask = 0;

        if (vmaf_init(&params->vmaf, configuration))
            v = avs_new_value_error("failed to initialize VMAF context");
    }

    if (!avs_defined(v))
    {
        for (int i = 0; i < numModel; ++i)
        {
            if (model[i] < 0 || model[i] > 3)
                v = avs_new_value_error("VMAF: model must be 0, 1, 2, or 3");

            if (!avs_defined(v) && std::count(model.get(), model.get() + numModel, model[i]) > 1)
                v = avs_new_value_error("VMAF: duplicate model specified");

            if (!avs_defined(v))
            {
                VmafModelConfig modelConfig{};
                modelConfig.name = modelName[model[i]];
                modelConfig.flags = VMAF_MODEL_FLAGS_DEFAULT;

                if (vmaf_model_load(&params->model[i], &modelConfig, modelVersion[model[i]]))
                {
                    params->modelCollection.resize(params->modelCollection.size() + 1);

                    if (vmaf_model_collection_load(&params->model[i], &params->modelCollection[params->modelCollection.size() - 1], &modelConfig, modelVersion[model[i]]))
                        v = avs_new_value_error(("VMAF: failed to load model: "s + modelVersion[model[i]]).c_str());

                    if (!avs_defined(v) && vmaf_use_features_from_model_collection(params->vmaf, params->modelCollection[params->modelCollection.size() - 1]))
                        v = avs_new_value_error(("VMAF: failed to load feature extractors from model collection: "s + modelVersion[model[i]]).c_str());

                    continue;
                }
            }

            if (!avs_defined(v) && vmaf_use_features_from_model(params->vmaf, params->model[i]))
                v = avs_new_value_error(("VMAF: failed to load feature extractors from model: "s + modelVersion[model[i]]).c_str());
        }
    }

    if (!avs_defined(v))
    {
        std::unique_ptr<int[]> feature;
        int numFeature = [&]() {
            if (avs_defined(avs_array_elt(args, 5)))
            {
                if (avs_is_array(avs_array_elt(args, 5)))
                    return avs_array_size(avs_array_elt(args, 5));
                else if (avs_is_int(avs_array_elt(args, 5)))
                    return -1;
            }
            else
                return 0;
        }();

        if (numFeature == -1)
        {
            feature = std::make_unique<int[]>(numFeature);

            for (int i = 0; i < numFeature; ++i)
                feature[i] = avs_is_int(*(avs_as_array(avs_array_elt(args, 5)) + i));
        }
        else if (numFeature > 0)
        {
            feature = std::make_unique<int[]>(1);
            feature[0] = (feature == 0) ? 0 : avs_as_int(avs_array_elt(args, 5));
            numFeature = 1;
        }

        for (int i = 0; i < numFeature; ++i)
        {
            if (feature[i] < 0 || feature[i] > 4)
                v = avs_new_value_error("VMAF: feature must be 0, 1, 2, 3, or 4");

            if (!avs_defined(v) && std::count(feature.get(), feature.get() + numFeature, feature[i]) > 1)
                v = avs_new_value_error("VMAF: duplicate feature specified");

            if (!avs_defined(v) && vmaf_use_feature(params->vmaf, featureName[feature[i]], nullptr))
                v = avs_new_value_error(("VMAF: failed to load feature extractor: "s + featureName[feature[i]]).c_str());

            if (!avs_defined(v))
            {
                if (!std::strcmp(featureName[feature[i]], "psnr") ||
                    !std::strcmp(featureName[feature[i]], "psnr_hvs") ||
                    !std::strcmp(featureName[feature[i]], "ciede"))
                    params->chroma = true;
            }
        }
    }

    if (!avs_defined(v))
    {
        if (avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) == 1 && avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U) == 1)
            params->pixelFormat = VMAF_PIX_FMT_YUV420P;
        else if (avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) == 1 && avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U) == 0)
            params->pixelFormat = VMAF_PIX_FMT_YUV422P;
        else
            params->pixelFormat = VMAF_PIX_FMT_YUV444P;

        v = avs_new_value_clip(clip);
    }

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = vmaf_get_frame;
    fi->free_filter = free_vmaf;

    avs_release_clip(clip);

    return v;
}

const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    avs_add_function(env, "VMAF", "ccs[log_format]i[model]i*[feature]i*", Create_VMAF, 0);
    return "VMAF";
}
