#include <thread>

#include "VMAF.h"

struct VMAF
{
    AVS_Clip *distorted;
    std::string logPath;
    VmafOutputFormat logFormat;
    std::vector<VmafModel *> model;
    std::vector<VmafModelCollection *> modelCollection;
    VmafContext *vmaf;
    VmafPixelFormat pixelFormat;
    bool chroma;
};

AVS_VideoFrame *AVSC_CC vmaf_get_frame(AVS_FilterInfo *fi, int n)
{
    const char *ErrorText = 0;
    VMAF *d = reinterpret_cast<VMAF *>(fi->user_data);

    AVS_VideoFrame *reference = avs_get_frame(fi->child, n);
    AVS_VideoFrame *distorted = avs_get_frame(d->distorted, n);

    VmafPicture ref{}, dist{};

    if (vmaf_picture_alloc(&ref, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height) ||
        vmaf_picture_alloc(&dist, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height))
        ErrorText = "VMAF: failed to allocate picture.";

    const int pl[3] = {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V};
    const int planecount = std::min(avs_num_components(&fi->vi), 3);
    for (int plane = 0; plane < planecount; ++plane)
    {
        if (plane && !d->chroma)
            break;

        avs_bit_blt(fi->env, reinterpret_cast<uint8_t *>(ref.data[plane]),
                    ref.stride[plane],
                    avs_get_read_ptr_p(reference, pl[plane]),
                    avs_get_pitch_p(reference, pl[plane]),
                    avs_get_row_size_p(reference, pl[plane]),
                    avs_get_height_p(reference, pl[plane]));

        avs_bit_blt(fi->env, reinterpret_cast<uint8_t *>(dist.data[plane]),
                    dist.stride[plane],
                    avs_get_read_ptr_p(distorted, pl[plane]),
                    avs_get_pitch_p(distorted, pl[plane]),
                    avs_get_row_size_p(distorted, pl[plane]),
                    avs_get_height_p(distorted, pl[plane]));
    }

    if (!ErrorText && vmaf_read_pictures(d->vmaf, &ref, &dist, n))
        ErrorText = "VMAF:failed to read pictures.";

    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);

    if (ErrorText)
    {
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

void AVSC_CC free_vmaf(AVS_FilterInfo *fi)
{
    const char *ErrorText = 0;
    VMAF *d = reinterpret_cast<VMAF *>(fi->user_data);

    avs_release_clip(d->distorted);

    if (vmaf_read_pictures(d->vmaf, nullptr, nullptr, 0))
        ErrorText = "VMAF:failed to flush context.";

    if (!ErrorText)
    {
        for (auto &&m : d->model)
            if (double score; vmaf_score_pooled(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, fi->vi.num_frames - 1))
                ErrorText = "VMAF:failed to generate pooled VMAF model score.";
    }

    if (!ErrorText)
    {
        for (auto &&m : d->modelCollection)
            if (VmafModelCollectionScore score; vmaf_score_pooled_model_collection(d->vmaf, m, VMAF_POOL_METHOD_MEAN, &score, 0, fi->vi.num_frames - 1))
                ErrorText = "VMAF:failed to generate pooled VMAF model collection score.";
    }

    if (!ErrorText)
    {
        if (vmaf_write_output(d->vmaf, d->logPath.c_str(), d->logFormat))
            ErrorText = "VMAF: failed to write VMAF stats.";
    }

    for (auto &&m : d->model)
        vmaf_model_destroy(m);
    for (auto &&m : d->modelCollection)
        vmaf_model_collection_destroy(m);
    vmaf_close(d->vmaf);

    delete d;

    if (ErrorText)
        std::cout << ErrorText;
}

static int AVSC_CC vmaf_set_cache_hints(AVS_FilterInfo *fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 3 : 0;
}

AVS_Value AVSC_CC Create_VMAF(AVS_ScriptEnvironment *env, AVS_Value args, void *param)
{
    AVS_FilterInfo *fi;

    AVS_Clip *clip = avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1);

    VMAF *params = new VMAF();

    params->distorted = avs_take_clip(avs_array_elt(args, 1), env);
    params->logPath = avs_as_string(avs_array_elt(args, 2));
    const int logFormat = (avs_is_int(avs_array_elt(args, 3))) ? avs_as_int(avs_array_elt(args, 3)) : 0;

    std::unique_ptr<int[]> model;
    const int numModel = (avs_defined(avs_array_elt(args, 4))) ? avs_array_size(avs_array_elt(args, 4)) : 0;

    if (numModel > 0)
    {
        model = std::make_unique<int[]>(numModel);

        for (int i = 0; i < numModel; ++i)
            model[i] = avs_as_int(*(avs_as_array(avs_array_elt(args, 4)) + i));

        params->model.resize(numModel);
    }

    AVS_Value v = avs_void;

    if (avs_bits_per_component(&fi->vi) > 10)
        v = avs_new_value_error("VMAF: only 8..10-bit input supported.");
    if (!avs_defined(v) && avs_is_rgb(&fi->vi))
        v = avs_new_value_error("VMAF: RGB color family is not supported.");
    if (!avs_defined(v) && !avs_is_planar(&fi->vi))
        v = avs_new_value_error("VMAF: only planar format supported.");
    if (!avs_defined(v) && avs_num_components(&fi->vi) < 3)
        v = avs_new_value_error("VMAF: minimum three planes are required.");

    const bool is420 = avs_is_420(&fi->vi);
    const bool is422 = avs_is_422(&fi->vi);
    const bool is444 = avs_is_444(&fi->vi);

    if (!avs_defined(v) && !(is420 || is422 || is444))
        v = avs_new_value_error("VMAF: only 420/422/444 chroma subsampling is supported.");

    if (!avs_defined(v))
    {
        const AVS_VideoInfo *vi1 = avs_get_video_info(params->distorted);

        if (!avs_is_same_colorspace(&fi->vi, vi1))
            v = avs_new_value_error("VMAF: both clips must be the same format.");
        if (!avs_defined(v) && fi->vi.width != vi1->width || fi->vi.height != vi1->height)
            v = avs_new_value_error("VMAF: both clips must have the same dimensions.");
        if (!avs_defined(v) && fi->vi.num_frames != vi1->num_frames)
            v = avs_new_value_error("VMAF: both clips' number of frames don't match.");
    }
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
            v = avs_new_value_error("VMAF:failed to initialize VMAF context.");
    }

    if (!avs_defined(v))
    {
        for (int i = 0; i < numModel; ++i)
        {
            if (model[i] < 0 || model[i] > 3)
                v = avs_new_value_error("VMAF: model must be 0, 1, 2, or 3.");

            if (!avs_defined(v) && std::count(model.get(), model.get() + numModel, model[i]) > 1)
                v = avs_new_value_error("VMAF: duplicate model specified.");

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
        const int numFeature = (avs_defined(avs_array_elt(args, 5))) ? avs_array_size(avs_array_elt(args, 5)) : 0;

        if (numFeature > 0)
        {
            feature = std::make_unique<int[]>(numFeature);

            for (int i = 0; i < numFeature; ++i)
                feature[i] = avs_as_int(*(avs_as_array(avs_array_elt(args, 5)) + i));
        }

        for (int i = 0; i < numFeature; ++i)
        {
            if (feature[i] < 0 || feature[i] > 5)
                v = avs_new_value_error("VMAF: feature must be 0, 1, 2, 3, 4 or 5.");

            if (!avs_defined(v) && std::count(feature.get(), feature.get() + numFeature, feature[i]) > 1)
                v = avs_new_value_error("VMAF: duplicate feature specified.");

            if (!avs_defined(v) && feature[i] == 5)
            {
                if (avs_defined(avs_array_elt(args, 6)))
                {
                    std::regex reg(R"((\w+)=([^ >]+)(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?)");
                    std::string cambi_opt = avs_as_string(avs_array_elt(args, 6));

                    std::smatch match;
                    if (!std::regex_match(cambi_opt.cbegin(), cambi_opt.cend(), match, reg))
                        v = avs_new_value_error("VMAF: cannot parse cambi_opt.");

                    if (!avs_defined(v))
                    {
                        VmafFeatureDictionary *featureDictionary{};
                        std::vector<int> unique_name;
                        unique_name.reserve(match.size());

                        for (int i = 1; i < match.size(); i += 2)
                        {
                            if (match[i].str() == "enc_width")
                            {
                                if (std::stoi(match[i + 1].str()) < 320 || std::stoi(match[i + 1].str()) > 7680)
                                {
                                    v = avs_new_value_error("VMAF: enc_width must be between 180..7680.");
                                    break;
                                }

                                unique_name.emplace_back(1);
                                if (std::count(unique_name.begin(), unique_name.end(), 1) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "enc_height")
                            {
                                if (std::stoi(match[i + 1].str()) < 200 || std::stoi(match[i + 1].str()) > 4320)
                                {
                                    v = avs_new_value_error("VMAF: enc_height must be between 180..7680.");
                                    break;
                                }

                                unique_name.emplace_back(2);
                                if (std::count(unique_name.begin(), unique_name.end(), 2) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "window_size")
                            {
                                if (std::stoi(match[i + 1].str()) < 15 || std::stoi(match[i + 1].str()) > 127)
                                {
                                    v = avs_new_value_error("VMAF: window_size must be between 15..127.");
                                    break;
                                }

                                unique_name.emplace_back(3);
                                if (std::count(unique_name.begin(), unique_name.end(), 3) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "topk")
                            {
                                if (std::stod(match[i + 1].str()) < 0.0001 || std::stod(match[i + 1].str()) > 1.0)
                                {
                                    v = avs_new_value_error("VMAF: topk must be between 0.0001..1.0.");
                                    break;
                                }

                                unique_name.emplace_back(4);
                                if (std::count(unique_name.begin(), unique_name.end(), 4) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "tvi_threshold")
                            {
                                if (std::stod(match[i + 1].str()) < 0.0001 || std::stod(match[i + 1].str()) > 1.0)
                                {
                                    v = avs_new_value_error("VMAF: tvi_threshold must be between 0.0001..1.0.");
                                    break;
                                }

                                unique_name.emplace_back(5);
                                if (std::count(unique_name.begin(), unique_name.end(), 5) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "max_log_contrast")
                            {
                                if (std::stoi(match[i + 1].str()) < 0 || std::stoi(match[i + 1].str()) > 5)
                                {
                                    v = avs_new_value_error("VMAF: max_log_contrast must be between 0..5.");
                                    break;
                                }

                                unique_name.emplace_back(6);
                                if (std::count(unique_name.begin(), unique_name.end(), 6) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "enc_bitdepth")
                            {
                                if (std::stoi(match[i + 1].str()) < 6 || std::stoi(match[i + 1].str()) > 16)
                                {
                                    v = avs_new_value_error("VMAF: enc_bitdepth must be between 6..16.");
                                    break;
                                }

                                unique_name.emplace_back(7);
                                if (std::count(unique_name.begin(), unique_name.end(), 7) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].str() == "eotf")
                            {
                                if (!(match[i + 1].str() == "bt1886" || match[i + 1].str() == "pq"))
                                {
                                    v = avs_new_value_error("VMAF: eotf has wrong value.");
                                    break;
                                }

                                unique_name.emplace_back(8);
                                if (std::count(unique_name.begin(), unique_name.end(), 8) > 1)
                                {
                                    v = avs_new_value_error("VMAF: duplicate cambi_opt specified");
                                    break;
                                }
                            }
                            else if (match[i].length())
                            {
                                static const std::string m = "VMAF: wrong cambi_opt " + match[i].str() + ".";
                                v = avs_new_value_error((m).c_str());
                                break;
                            }
                            else
                                break;
                        }

                        if (!avs_defined(v))
                        {
                            for (int i = 1; i < match.size(); i += 2)
                            {
                                if (match[i].length() == 0)
                                    break;

                                if (vmaf_feature_dictionary_set(&featureDictionary, match[i].str().c_str(), match[i + 1].str().c_str()))
                                {
                                    static const std::string m = "VMAF: failed to set cambi option "s + match[i].str() + "."s;
                                    v = avs_new_value_error((m).c_str());
                                    break;
                                }
                            }
                        }

                        if (!avs_defined(v) && vmaf_use_feature(params->vmaf, "cambi", featureDictionary))
                        {
                            vmaf_feature_dictionary_free(&featureDictionary);
                            v = avs_new_value_error("VMAF: failed to load feature extractor: cambi.");
                        }
                    }
                }
                else
                {
                    if (vmaf_use_feature(params->vmaf, "cambi", nullptr))
                        v = avs_new_value_error("VMAF: failed to load feature extractor: cambi.");
                }
            }

            if (!avs_defined(v) && feature[i] != 5 && vmaf_use_feature(params->vmaf, featureName[feature[i]], nullptr))
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
        if (is420)
            params->pixelFormat = VMAF_PIX_FMT_YUV420P;
        else if (is422)
            params->pixelFormat = VMAF_PIX_FMT_YUV422P;
        else
            params->pixelFormat = VMAF_PIX_FMT_YUV444P;

        v = avs_new_value_clip(clip);
    }

    fi->user_data = reinterpret_cast<void *>(params);
    fi->get_frame = vmaf_get_frame;
    fi->set_cache_hints = vmaf_set_cache_hints;
    fi->free_filter = free_vmaf;

    avs_release_clip(clip);

    return v;
}
