#include "VMAF.h"

struct VMAF2
{
    AVS_Clip* distorted;
    VmafPixelFormat pixelFormat;
    bool chroma;
    int numFeature;
    std::vector<int> feature;
    std::vector<const char*> featureN;
    std::vector<std::string> match;
    int f;
};

AVS_VideoFrame* AVSC_CC vmaf2_get_frame(AVS_FilterInfo* fi, int n)
{
    const char* ErrorText = 0;
    VMAF2* d = reinterpret_cast<VMAF2*>(fi->user_data);

    VmafConfiguration configuration {};
    configuration.log_level = VMAF_LOG_LEVEL_NONE;
    configuration.n_threads = 0;
    configuration.n_subsample = 1;
    configuration.cpumask = 0;

    VmafContext* vmaf;

    if (vmaf_init(&vmaf, configuration))
        ErrorText = "VMAF2: failed to initialize VMAF2 context.";

    if (!ErrorText)
    {
        for (int i = 0; i < d->numFeature; ++i)
        {
            if (!ErrorText && d->feature[i] == 5)
            {
                if (d->f)
                {
                    VmafFeatureDictionary* featureDictionary {};

                    for (int i = 1; i < d->match.size(); i += 2)
                    {
                        if (d->match[i].length() == 0)
                            break;

                        if (vmaf_feature_dictionary_set(&featureDictionary, d->match[i].c_str(), d->match[i + 1].c_str()))
                        {
                            static const std::string m = "VMAF2: failed to set cambi option "s + d->match[i] + "."s;
                            ErrorText = (m).c_str();
                            break;
                        }
                    }

                    if (!ErrorText && vmaf_use_feature(vmaf, "cambi", featureDictionary))
                    {
                        vmaf_feature_dictionary_free(&featureDictionary);
                        ErrorText = "VMAF2: failed to load feature extractor: cambi.";
                    }

                    if (!ErrorText && d->feature[i] != 5 && vmaf_use_feature(vmaf, featureName[d->feature[i]], nullptr))
                        ErrorText = ("VMAF2: failed to load feature extractor: "s + featureName[d->feature[i]]).c_str();
                }
                else
                {
                    if (vmaf_use_feature(vmaf, "cambi", nullptr))
                        ErrorText = "VMAF2: failed to load feature extractor: cambi.";
                }
            }

            if (!ErrorText && d->feature[i] != 5 && vmaf_use_feature(vmaf, featureName[d->feature[i]], nullptr))
                ErrorText = ("VMAF2: failed to load feature extractor: "s + featureName[d->feature[i]]).c_str();

            if (!ErrorText)
            {
                if (!std::strcmp(featureName[d->feature[i]], "psnr") ||
                    !std::strcmp(featureName[d->feature[i]], "psnr_hvs") ||
                    !std::strcmp(featureName[d->feature[i]], "ciede"))
                    d->chroma = true;
            }
        }

    }

    AVS_VideoFrame* reference = avs_get_frame(fi->child, n);
    if (!reference)
    {
        vmaf_close(vmaf);
        return nullptr;
    }

    AVS_VideoFrame* distorted = avs_get_frame(d->distorted, n);
    if (!distorted)
    {
        vmaf_close(vmaf);
        return nullptr;
    }

    VmafPicture ref{};
    VmafPicture dist{};

    if (vmaf_picture_alloc(&ref, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height) ||
        vmaf_picture_alloc(&dist, d->pixelFormat, avs_bits_per_component(&fi->vi), fi->vi.width, fi->vi.height))
        ErrorText = "VMAF2: failed to allocate picture.";

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

    if (!ErrorText && vmaf_read_pictures(vmaf, &ref, &dist, n))
        ErrorText = "VMAF2: failed to read pictures";

    if (vmaf_read_pictures(vmaf, nullptr, nullptr, 0))
        ErrorText = "VMAF2: failed to flush context";

    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);

    if (!ErrorText && d->numFeature > 0)
    {
        for (int i = 0; i < d->featureN.size(); ++i)
        {
            double score = -1;

            if (vmaf_feature_score_at_index(vmaf, d->featureN[i], &score, n))
            {
                ErrorText = "VMAF2: failed to generate pooled VMAF2 feature score.";
                break;
            }

            avs_prop_set_float(fi->env, avs_get_frame_props_rw(fi->env, reference), d->featureN[i], score, 0);
        }
    }

    vmaf_close(vmaf);

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

void AVSC_CC free_vmaf2(AVS_FilterInfo* fi)
{
    VMAF2* d = reinterpret_cast<VMAF2*>(fi->user_data);

    avs_release_clip(d->distorted);

    delete d;
}

static int AVSC_CC vmaf2_set_cache_hints(AVS_FilterInfo *fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 1 : 0;
}

AVS_Value AVSC_CC Create_VMAF2(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    AVS_FilterInfo* fi;

    AVS_Clip* clip = avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1);

    VMAF2* params = new VMAF2();

    params->numFeature = (avs_defined(avs_array_elt(args, 2))) ? avs_array_size(avs_array_elt(args, 2)) : 0;

    AVS_Value v = avs_void;

    if (avs_bits_per_component(&fi->vi) > 10)
        v = avs_new_value_error("VMAF2: only 8..10-bit input supported.");
    if (!avs_defined(v) && avs_is_rgb(&fi->vi))
        v = avs_new_value_error("VMAF2: RGB color family is not supported.");
    if (!avs_defined(v) && !avs_is_planar(&fi->vi))
        v = avs_new_value_error("VMAF2: only planar format supported.");
    if (!avs_defined(v) && avs_num_components(&fi->vi) < 3)
        v = avs_new_value_error("VMAF2: minimum three planes are required.");

    const bool is420 = avs_is_420(&fi->vi);
    const bool is422 = avs_is_422(&fi->vi);
    const bool is444 = avs_is_444(&fi->vi);

    if (!avs_defined(v) && !(is420 || is422 || is444))
        v = avs_new_value_error("VMAF2: only 420/422/444 chroma subsampling is supported.");

    if (!avs_defined(v))
    {
        if (params->numFeature > 0)
        {
            params->feature.reserve(params->numFeature);
            params->featureN.reserve(4);

            for (int i = 0; i < params->numFeature; ++i)
                params->feature.emplace_back(avs_as_int(*(avs_as_array(avs_array_elt(args, 2)) + i)));

            if (avs_defined(avs_array_elt(args, 1)))
            {
                params->distorted = avs_take_clip(avs_array_elt(args, 1), env);
                const AVS_VideoInfo *vi1 = avs_get_video_info(params->distorted);

                if (!avs_is_same_colorspace(&fi->vi, vi1))
                    v = avs_new_value_error("VMAF2: both clips must be the same format.");
                if (!avs_defined(v) && fi->vi.width != vi1->width || fi->vi.height != vi1->height)
                    v = avs_new_value_error("VMAF2: both clips must have the same dimensions.");
                if (!avs_defined(v) && fi->vi.num_frames != vi1->num_frames)
                    v = avs_new_value_error("VMAF2: both clips' number of frames don't match.");
            }
        }
    }

    if (!avs_defined(v))
    {
        for (int i = 0; i < params->numFeature; ++i)
        {
            if (params->feature[i] < 0 || params->feature[i] > 5)
                v = avs_new_value_error("VMAF2: feature must be 0, 1, 2, 3, 4 or 5");

            if (!avs_defined(v) && std::count(params->feature.begin(), params->feature.end(), params->feature[i]) > 1)
                v = avs_new_value_error("VMAF2: duplicate feature specified");

            switch (params->feature[i])
            {
            case 0:
                params->featureN.emplace_back("psnr_y");
                params->featureN.emplace_back("psnr_cb");
                params->featureN.emplace_back("psnr_cr");
                break;
            case 1:
                params->featureN.emplace_back("psnr_hvs_y");
                params->featureN.emplace_back("psnr_hvs_cb");
                params->featureN.emplace_back("psnr_hvs_cr");
                params->featureN.emplace_back("psnr_hvs");
                break;
            case 2:
                params->featureN.emplace_back("float_ssim");
                break;
            case 3:
                params->featureN.emplace_back("float_ms_ssim");
                break;
            case 4:
                params->featureN.emplace_back("ciede2000");
                break;
            case 5:
                params->featureN.emplace_back("cambi");
                break;
            }

            if (!avs_defined(v) && params->feature[i] == 5)
            {
                if (params->feature.size() > 1)
                    v = avs_new_value_error("VMAF2: cambi cannot be used together with other feature.");

                if (!avs_defined(v))
                {
                    params->distorted = avs_take_clip(avs_array_elt(args, 0), env);
                    params->f = avs_defined(avs_array_elt(args, 3));

                    if (params->f)
                    {
                        std::regex reg(R"((\w+)=([^ >]+)(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?(?: (\w+)(?:=([^ >]+)))?)");
                        std::string cambi_opt = avs_as_string(avs_array_elt(args, 3));

                        std::smatch match;
                        if (!std::regex_match(cambi_opt.cbegin(), cambi_opt.cend(), match, reg))
                            v = avs_new_value_error("VMAF2: cannot parse cambi_opt.");

                        params->match.reserve(match.size());
                        for (std::string match : match)
                            params->match.emplace_back(match);

                        if (!avs_defined(v))
                        {
                            std::vector<int> unique_name;
                            unique_name.reserve(match.size());

                            for (int i = 1; match[i + 1].matched; i += 2)
                            {
                                if (match[i].str() == "enc_width")
                                {
                                    if (std::stoi(match[i + 1].str()) < 320 || std::stoi(match[i + 1].str()) > 7680)
                                    {
                                        v = avs_new_value_error("VMAF2: enc_width must be between 180..7680.");
                                        break;
                                    }

                                    unique_name.emplace_back(1);
                                    if (std::count(unique_name.begin(), unique_name.end(), 1) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "enc_height")
                                {
                                    if (std::stoi(match[i + 1].str()) < 200 || std::stoi(match[i + 1].str()) > 4320)
                                    {
                                        v = avs_new_value_error("VMAF2: enc_height must be between 180..7680.");
                                        break;
                                    }

                                    unique_name.emplace_back(2);
                                    if (std::count(unique_name.begin(), unique_name.end(), 2) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "window_size")
                                {
                                    if (std::stoi(match[i + 1].str()) < 15 || std::stoi(match[i + 1].str()) > 127)
                                    {
                                        v = avs_new_value_error("VMAF2: window_size must be between 15..127.");
                                        break;
                                    }

                                    unique_name.emplace_back(3);
                                    if (std::count(unique_name.begin(), unique_name.end(), 3) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "topk")
                                {
                                    if (std::stod(match[i + 1].str()) < 0.0001 || std::stod(match[i + 1].str()) > 1.0)
                                    {
                                        v = avs_new_value_error("VMAF2: topk must be between 0.0001..1.0.");
                                        break;
                                    }

                                    unique_name.emplace_back(4);
                                    if (std::count(unique_name.begin(), unique_name.end(), 4) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "tvi_threshold")
                                {
                                    if (std::stod(match[i + 1].str()) < 0.0001 || std::stod(match[i + 1].str()) > 1.0)
                                    {
                                        v = avs_new_value_error("VMAF2: tvi_threshold must be between 0.0001..1.0.");
                                        break;
                                    }

                                    unique_name.emplace_back(5);
                                    if (std::count(unique_name.begin(), unique_name.end(), 5) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "max_log_contrast")
                                {
                                    if (std::stoi(match[i + 1].str()) < 0 || std::stoi(match[i + 1].str()) > 5)
                                    {
                                        v = avs_new_value_error("VMAF2: max_log_contrast must be between 0..5.");
                                        break;
                                    }

                                    unique_name.emplace_back(6);
                                    if (std::count(unique_name.begin(), unique_name.end(), 6) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "enc_bitdepth")
                                {
                                    if (std::stoi(match[i + 1].str()) < 6 || std::stoi(match[i + 1].str()) > 16)
                                    {
                                        v = avs_new_value_error("VMAF2: enc_bitdepth must be between 6..16.");
                                        break;
                                    }

                                    unique_name.emplace_back(7);
                                    if (std::count(unique_name.begin(), unique_name.end(), 7) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].str() == "eotf")
                                {
                                    if (!(match[i + 1].str() == "bt1886" || match[i + 1].str() == "pq"))
                                    {
                                        v = avs_new_value_error("VMAF2: eotf has wrong value.");
                                        break;
                                    }

                                    unique_name.emplace_back(8);
                                    if (std::count(unique_name.begin(), unique_name.end(), 8) > 1)
                                    {
                                        v = avs_new_value_error("VMAF2: duplicate cambi_opt specified");
                                        break;
                                    }
                                }
                                else if (match[i].length())
                                {
                                    static const std::string m = "VMAF2: wrong cambi_opt " + match[i].str() + ".";
                                    v = avs_new_value_error((m).c_str());
                                    break;
                                }
                                else
                                    break;
                            }
                        }
                    }
                }
            }
            else
            {
                if (!avs_defined(avs_array_elt(args, 1)))
                    v = avs_new_value_error("VMAF2: distorted clip must be specified.");
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

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = vmaf2_get_frame;
    fi->set_cache_hints = vmaf2_set_cache_hints;
    fi->free_filter = free_vmaf2;

    avs_release_clip(clip);

    return v;
}
