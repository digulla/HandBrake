/* encavcodec.c

   Copyright (c) 2003-2019 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "hb.h"
#include "hb_dict.h"
#include "hbffmpeg.h"
#include "h264_common.h"
#include "h265_common.h"
#include "nal_units.h"

/*
 * The frame info struct remembers information about each frame across calls
 * to avcodec_encode_video. Since frames are uniquely identified by their
 * frame number, we use this as an index.
 *
 * The size of the array is chosen so that two frames can't use the same
 * slot during the encoder's max frame delay (set by the standard as 16
 * frames) and so that, up to some minimum frame rate, frames are guaranteed
 * to map to * different slots.
 */
#define FRAME_INFO_SIZE 32
#define FRAME_INFO_MASK (FRAME_INFO_SIZE - 1)

struct hb_work_private_s
{
    hb_job_t           * job;
    AVCodecContext     * context;
    FILE               * file;

    int                  frameno_in;
    int                  frameno_out;
    hb_buffer_list_t     delay_list;

    int64_t              dts_delay;

    struct {
        int64_t          start;
        int64_t          duration;
    } frame_info[FRAME_INFO_SIZE];

    hb_chapter_queue_t * chapter_queue;
};

int  encavcodecInit( hb_work_object_t *, hb_job_t * );
int  encavcodecWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encavcodecClose( hb_work_object_t * );

static int apply_encoder_preset(int vcodec, AVDictionary ** av_opts,
                                const char * preset);

hb_work_object_t hb_encavcodec =
{
    WORK_ENCAVCODEC,
    "FFMPEG encoder (libavcodec)",
    encavcodecInit,
    encavcodecWork,
    encavcodecClose
};

static const char * const vpx_preset_names[] =
{
    "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL
};

static const char * const h26x_nvenc_preset_names[] =
{
    "hq", "hp", "fast", "medium", "slow", "default", NULL  // No Lossless "losslesshp", "lossless", "llhp", "llhq", "ll", "bd". We don't need them.
};

static const char * const h264_nvenc_profile_names[] =
{
    "auto", "baseline", "main", "high", NULL  // "high444p" not supported. 
};

static const char * const h265_nvenc_profile_names[] =
{
    "auto", "main", NULL // "main10", "rext"  We do not currently support 10bit encodes with this encoder. 
};

static const char * const h26x_vt_preset_name[] =
{
    "default", NULL
};

static const char * const h264_vt_profile_name[] =
{
    "auto", "baseline", "main", "high", NULL
};

static const char * const h265_vt_profile_name[] =
{
    "auto", "main",  NULL // "main10" not currently supported. 
};

int encavcodecInit( hb_work_object_t * w, hb_job_t * job )
{
    int ret = 0;
    char reason[80];
    char * codec_name = NULL;
    AVCodec * codec = NULL;
    AVCodecContext * context;
    AVRational fps;

    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data   = pv;
    pv->job           = job;
    pv->chapter_queue = hb_chapter_queue_init();

    hb_buffer_list_clear(&pv->delay_list);

    int clock_min, clock_max, clock;
    hb_video_framerate_get_limits(&clock_min, &clock_max, &clock);

    switch ( w->codec_param )
    {
        case AV_CODEC_ID_MPEG4:
        {
            hb_log("encavcodecInit: MPEG-4 ASP encoder");
            codec_name = "mpeg4";
        } break;
        case AV_CODEC_ID_MPEG2VIDEO:
        {
            hb_log("encavcodecInit: MPEG-2 encoder");
            codec_name = "mpeg2video";
        } break;
        case AV_CODEC_ID_VP8:
        {
            hb_log("encavcodecInit: VP8 encoder");
            codec_name = "libvpx";
        } break;
        case AV_CODEC_ID_VP9:
        {
            hb_log("encavcodecInit: VP9 encoder");
            codec_name = "libvpx-vp9";
        } break;
        case AV_CODEC_ID_H264:
        {
            switch (job->vcodec) {
                case HB_VCODEC_FFMPEG_NVENC_H264:
                    hb_log("encavcodecInit: H.264 (Nvidia NVENC)");
                    codec_name = "h264_nvenc";
                    break;
                case HB_VCODEC_FFMPEG_VCE_H264:
                    hb_log("encavcodecInit: H.264 (AMD VCE)");
                    codec_name = "h264_amf";
                    break;
                case HB_VCODEC_FFMPEG_VT_H264:
                    hb_log("encavcodecInit: H.264 (VideoToolbox)");
                    codec_name = "h264_videotoolbox";
                    break;
            }
        }break;
        case AV_CODEC_ID_HEVC:
        {
            switch (job->vcodec) {
                case HB_VCODEC_FFMPEG_NVENC_H265:
                    hb_log("encavcodecInit: H.265 (Nvidia NVENC)");
                    codec_name = "hevc_nvenc";
                    break;
                case HB_VCODEC_FFMPEG_VCE_H265:
                    hb_log("encavcodecInit: H.265 (AMD VCE)");
                    codec_name = "hevc_amf";
                    break;
                case HB_VCODEC_FFMPEG_VT_H265:
                    hb_log("encavcodecInit: H.265 (VideoToolbox)");
                    codec_name = "hevc_videotoolbox";
                    break;
            }
        }break;
        default:
        {
            hb_error("encavcodecInit: unsupported encoder!");
            ret = 1;
            goto done;
        }
    }

    codec = avcodec_find_encoder_by_name(codec_name);
    if( !codec )
    {
        hb_log( "encavcodecInit: avcodec_find_encoder_by_name(%s) "
                "failed", codec_name );
        ret = 1;
        goto done;
    }
    context = avcodec_alloc_context3( codec );

    // Set things in context that we will allow the user to
    // override with advanced settings.
    fps.den = job->vrate.den;
    fps.num = job->vrate.num;

    // If the fps.num is the internal clock rate, there's a good chance
    // this is a standard rate that we have in our hb_video_rates table.
    // Because of rounding errors and approximations made while
    // measuring framerate, the actual value may not be exact.  So
    // we look for rates that are "close" and make an adjustment
    // to fps.den.
    if (fps.num == clock)
    {
        const hb_rate_t *video_framerate = NULL;
        while ((video_framerate = hb_video_framerate_get_next(video_framerate)) != NULL)
        {
            if (abs(fps.den - video_framerate->rate) < 10)
            {
                fps.den = video_framerate->rate;
                break;
            }
        }
    }
    hb_reduce(&fps.den, &fps.num, fps.den, fps.num);

    // Check that the framerate is supported.  If not, pick the closest.
    // The mpeg2 codec only supports a specific list of frame rates.
    if (codec->supported_framerates)
    {
        AVRational supported_fps;
        supported_fps = codec->supported_framerates[av_find_nearest_q_idx(fps, codec->supported_framerates)];
        if (supported_fps.num != fps.num || supported_fps.den != fps.den)
        {
            hb_log( "encavcodec: framerate %d / %d is not supported. Using %d / %d.",
                    fps.num, fps.den, supported_fps.num, supported_fps.den );
            fps = supported_fps;
        }
    }
    else if ((fps.num & ~0xFFFF) || (fps.den & ~0xFFFF))
    {
        // This may only be required for mpeg4 video. But since
        // our only supported options are mpeg2 and mpeg4, there is
        // no need to check codec type.
        hb_log( "encavcodec: truncating framerate %d / %d",
                fps.num, fps.den );
        while ((fps.num & ~0xFFFF) || (fps.den & ~0xFFFF))
        {
            fps.num >>= 1;
            fps.den >>= 1;
        }
    }

    context->time_base.den = fps.num;
    context->time_base.num = fps.den;
    context->gop_size  = ((double)job->orig_vrate.num / job->orig_vrate.den +
                                  0.5) * 10;
    if ((job->vcodec == HB_VCODEC_FFMPEG_VCE_H264) || (job->vcodec == HB_VCODEC_FFMPEG_VCE_H265))
    {
        // Set encoder preset
        context->profile = FF_PROFILE_UNKNOWN;
        if (job->encoder_preset != NULL && *job->encoder_preset)
        {
            if ((!strcasecmp(job->encoder_preset, "balanced"))
                || (!strcasecmp(job->encoder_preset, "speed"))
                || (!strcasecmp(job->encoder_preset, "quality")))
            {
                av_opt_set(context, "quality", job->encoder_preset, AV_OPT_SEARCH_CHILDREN);
            }
        }
    }

    /* place job->encoder_options in an hb_dict_t for convenience */
    hb_dict_t * lavc_opts = NULL;
    if (job->encoder_options != NULL && *job->encoder_options)
    {
        lavc_opts = hb_encopts_to_dict(job->encoder_options, job->vcodec);
    }

    AVDictionary * av_opts = NULL;
    if (apply_encoder_preset(job->vcodec, &av_opts, job->encoder_preset))
    {
        av_free( context );
        av_dict_free( &av_opts );
        ret = 1;
        goto done;
    }

    /* iterate through lavc_opts and have avutil parse the options for us */
    hb_dict_iter_t iter;
    for (iter  = hb_dict_iter_init(lavc_opts);
         iter != HB_DICT_ITER_DONE;
         iter  = hb_dict_iter_next(lavc_opts, iter))
    {
        const char *key = hb_dict_iter_key(iter);
        hb_value_t *value = hb_dict_iter_value(iter);
        char *str = hb_value_get_string_xform(value);

        /* Here's where the strings are passed to avutil for parsing. */
        av_dict_set( &av_opts, key, str, 0 );
        free(str);
    }
    hb_dict_free( &lavc_opts );

    // Now set the things in context that we don't want to allow
    // the user to override.
    if (job->vquality <= HB_INVALID_VIDEO_QUALITY)
    {
        /* Average bitrate */
        context->bit_rate = 1000 * job->vbitrate;
        // ffmpeg's mpeg2 encoder requires that the bit_rate_tolerance be >=
        // bitrate * fps
        context->bit_rate_tolerance = context->bit_rate * av_q2d(fps) + 1;
        
        if ( job->vcodec == HB_VCODEC_FFMPEG_NVENC_H264 ||
                  job->vcodec == HB_VCODEC_FFMPEG_NVENC_H265 ) {
            av_dict_set( &av_opts, "rc", "cbr_hq", 0 );
            hb_log( "encavcodec: encoding at rc=cbr_hq Bitrate %d", job->vbitrate );
        }
    }
    else
    {
        /* Constant quantizer */

        //Set constant quality for libvpx
        if ( w->codec_param == AV_CODEC_ID_VP8 ||
             w->codec_param == AV_CODEC_ID_VP9 )
        {
            // These settings produce better image quality than
            // what was previously used
            context->flags |= AV_CODEC_FLAG_QSCALE;
            context->global_quality = FF_QP2LAMBDA * job->vquality + 0.5;
        
            char quality[7];
            snprintf(quality, 7, "%.2f", job->vquality);
            av_dict_set( &av_opts, "crf", quality, 0 );
            //This value was chosen to make the bitrate high enough
            //for libvpx to "turn off" the maximum bitrate feature
            //that is normally applied to constant quality.
            context->bit_rate = (int64_t)job->width * job->height *
                                         fps.num / fps.den;
            hb_log( "encavcodec: encoding at CQ %.2f", job->vquality );
        }
        //Set constant quality for nvenc
        else if ( job->vcodec == HB_VCODEC_FFMPEG_NVENC_H264 ||
                  job->vcodec == HB_VCODEC_FFMPEG_NVENC_H265 )
        {
            char qualityI[7];
            char quality[7];
            char qualityB[7];

            double adjustedQualityI = job->vquality - 2;
            double adjustedQualityB = job->vquality + 2;
            if (adjustedQualityB > 51) {
                adjustedQualityB = 51;
            }

            if (adjustedQualityI < 0){
                adjustedQualityI = 0;
            }

            snprintf(quality, 7, "%.2f", job->vquality);
            snprintf(qualityI, 7, "%.2f", adjustedQualityI);
            snprintf(qualityB, 7, "%.2f", adjustedQualityB);

            context->bit_rate = 0;

            av_dict_set( &av_opts, "rc", "vbr_hq", 0 );
            av_dict_set( &av_opts, "cq", quality, 0 );
            av_dict_set( &av_opts, "qmin", quality, 0 );
            av_dict_set( &av_opts, "qmax", quality, 0 );

            // further Advanced Quality Settings in Constant Quality Mode
            av_dict_set( &av_opts, "init_qpP", quality, 0 );
            av_dict_set( &av_opts, "init_qpB", qualityB, 0 );
            av_dict_set( &av_opts, "init_qpI", qualityI, 0 );
            hb_log( "encavcodec: encoding at rc=vbr %.2f", job->vquality );

            // Force IDR frames when we force a new keyframe for chapters
            av_dict_set( &av_opts, "forced-idr", "1", 0 );

        }
        else if ( job->vcodec == HB_VCODEC_FFMPEG_VCE_H264 || job->vcodec == HB_VCODEC_FFMPEG_VCE_H265 )
        {
            char quality[7];
            char qualityB[7];
            double adjustedQualityB = job->vquality + 2;

            snprintf(quality, 7, "%.2f", job->vquality);
            snprintf(qualityB, 7, "%.2f", adjustedQualityB);
            
            if (adjustedQualityB > 51) {
                adjustedQualityB = 51;
            }

            av_dict_set( &av_opts, "rc", "cqp", 0 );
           
            av_dict_set( &av_opts, "qp_i", quality, 0 );
            av_dict_set( &av_opts, "qp_p", quality, 0 );
            
            if ( job->vcodec != HB_VCODEC_FFMPEG_VCE_H265 )
            {
                av_dict_set( &av_opts, "qp_b", qualityB, 0 );
            }
            hb_log( "encavcodec: encoding at QP %.2f", job->vquality );
        }
        else
        {
            // These settings produce better image quality than
            // what was previously used
            context->flags |= AV_CODEC_FLAG_QSCALE;
            context->global_quality = FF_QP2LAMBDA * job->vquality + 0.5;
            
            hb_log( "encavcodec: encoding at constant quantizer %d",
                    context->global_quality );
        }
    }
    context->width     = job->width;
    context->height    = job->height;
    context->pix_fmt   = AV_PIX_FMT_YUV420P;

    context->sample_aspect_ratio.num = job->par.num;
    context->sample_aspect_ratio.den = job->par.den;
    if (job->vcodec == HB_VCODEC_FFMPEG_MPEG4)
    {
        // MPEG-4 Part 2 stores the PAR num/den as unsigned 8-bit fields,
        // and libavcodec's encoder fails to initialize if we don't
        // reduce it to fit 8-bits.
        hb_limit_rational(&context->sample_aspect_ratio.num,
                          &context->sample_aspect_ratio.den,
                           context->sample_aspect_ratio.num,
                           context->sample_aspect_ratio.den, 255);
    }

    hb_log( "encavcodec: encoding with stored aspect %d/%d",
            job->par.num, job->par.den );

    // set colorimetry
    context->color_primaries = hb_output_color_prim(job);
    context->color_trc       = hb_output_color_transfer(job);
    context->colorspace      = hb_output_color_matrix(job);

    if (!job->inline_parameter_sets)
    {
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if( job->grayscale )
    {
        context->flags |= AV_CODEC_FLAG_GRAY;
    }

    if (job->vcodec == HB_VCODEC_FFMPEG_VT_H264)
    {
        // Set profile and level
        if (job->encoder_profile != NULL && *job->encoder_profile)
        {
            if (!strcasecmp(job->encoder_profile, "baseline"))
                av_dict_set(&av_opts, "profile", "baseline", 0);
            else if (!strcasecmp(job->encoder_profile, "main"))
                av_dict_set(&av_opts, "profile", "main", 0);
            else if (!strcasecmp(job->encoder_profile, "high"))
                av_dict_set(&av_opts, "profile", "high", 0);
        }

        if (job->encoder_level != NULL && *job->encoder_level)
        {
            int i = 1;
            while (hb_h264_level_names[i] != NULL)
            {
                if (!strcasecmp(job->encoder_level, hb_h264_level_names[i]))
                    av_dict_set(&av_opts, "level", job->encoder_level, 0);
                ++i;
            }
        }

        context->max_b_frames = 16;
    }

    if (job->vcodec == HB_VCODEC_FFMPEG_VT_H265)
    {
        // Set profile and level
        if (job->encoder_profile != NULL && *job->encoder_profile)
        {
            if (!strcasecmp(job->encoder_profile, "main"))
                av_dict_set(&av_opts, "profile", "main", 0);
            else if (!strcasecmp(job->encoder_profile, "main10"))
                av_dict_set(&av_opts, "profile", "main10", 0);
        }

        context->max_b_frames = 16;
    }

    if (job->vcodec == HB_VCODEC_FFMPEG_VCE_H264)
    {
        // Set profile and level
        context->profile = FF_PROFILE_UNKNOWN;
        if (job->encoder_profile != NULL && *job->encoder_profile)
        {
            if (!strcasecmp(job->encoder_profile, "baseline"))
                context->profile = FF_PROFILE_H264_BASELINE;
            else if (!strcasecmp(job->encoder_profile, "main"))
                 context->profile = FF_PROFILE_H264_MAIN;
            else if (!strcasecmp(job->encoder_profile, "high"))
                context->profile = FF_PROFILE_H264_HIGH;
        }
        context->level = FF_LEVEL_UNKNOWN;
        if (job->encoder_level != NULL && *job->encoder_level)
        {
            int i = 1;
            while (hb_h264_level_names[i] != NULL)
            {
                if (!strcasecmp(job->encoder_level, hb_h264_level_names[i]))
                    context->level = hb_h264_level_values[i];
                ++i;
            }
        }
    }

    if (job->vcodec == HB_VCODEC_FFMPEG_VCE_H265)
    {
        // Set profile and level
        context->profile = FF_PROFILE_UNKNOWN;
        if (job->encoder_profile != NULL && *job->encoder_profile)
        {
            if (!strcasecmp(job->encoder_profile, "main"))
                 context->profile = FF_PROFILE_HEVC_MAIN;
        }
        context->level = FF_LEVEL_UNKNOWN;
        if (job->encoder_level != NULL && *job->encoder_level)
        {
            int i = 1;
            while (hb_h265_level_names[i] != NULL)
            {
                if (!strcasecmp(job->encoder_level, hb_h265_level_names[i]))
                    context->level = hb_h265_level_values[i];
                ++i;
            }
        }
        // FIXME
        //context->tier = FF_TIER_UNKNOWN;
    }
    
    if ( job->vcodec == HB_VCODEC_FFMPEG_NVENC_H264 || job->vcodec == HB_VCODEC_FFMPEG_NVENC_H265 )
    {
        // Set profile and level
        if (job->encoder_profile != NULL && *job->encoder_profile)
        {
            if (!strcasecmp(job->encoder_profile, "baseline"))
                av_dict_set(&av_opts, "profile", "baseline", 0);
            else if (!strcasecmp(job->encoder_profile, "main"))
                av_dict_set(&av_opts, "profile", "main", 0);
            else if (!strcasecmp(job->encoder_profile, "high"))
                av_dict_set(&av_opts, "profile", "high", 0);
        }

        if (job->encoder_level != NULL && *job->encoder_level)
        {
            int i = 1;
            while (hb_h264_level_names[i] != NULL)
            {
                if (!strcasecmp(job->encoder_level, hb_h264_level_names[i]))
                    av_dict_set(&av_opts, "level", job->encoder_level, 0);
                ++i;
            }
        }
    }

    // Make VCE h.265 encoder emit an IDR for every GOP
    if (job->vcodec == HB_VCODEC_FFMPEG_VCE_H265)
    {
        av_dict_set(&av_opts, "gops_per_idr", "1", 0);
    }

    if( job->pass_id == HB_PASS_ENCODE_1ST ||
        job->pass_id == HB_PASS_ENCODE_2ND )
    {
        char * filename = hb_get_temporary_filename("ffmpeg.log");

        if( job->pass_id == HB_PASS_ENCODE_1ST )
        {
            pv->file = hb_fopen(filename, "wb");
            if (!pv->file)
            {
                if (strerror_r(errno, reason, 79) != 0)
                    strcpy(reason, "unknown -- strerror_r() failed");

                hb_error("encavcodecInit: Failed to open %s (reason: %s)", filename, reason);
                free(filename);
                ret = 1;
                goto done;
            }
            context->flags |= AV_CODEC_FLAG_PASS1;
        }
        else
        {
            int    size;
            char * log;

            pv->file = hb_fopen(filename, "rb");
            if (!pv->file) {
                if (strerror_r(errno, reason, 79) != 0)
                    strcpy(reason, "unknown -- strerror_r() failed");

                hb_error("encavcodecInit: Failed to open %s (reason: %s)", filename, reason);
                free(filename);
                ret = 1;
                goto done;
            }
            fseek( pv->file, 0, SEEK_END );
            size = ftell( pv->file );
            fseek( pv->file, 0, SEEK_SET );
            log = malloc( size + 1 );
            log[size] = '\0';
            if (size > 0 &&
                fread( log, size, 1, pv->file ) < size)
            {
                if (ferror(pv->file))
                {
                    if (strerror_r(errno, reason, 79) != 0)
                        strcpy(reason, "unknown -- strerror_r() failed");

                    hb_error( "encavcodecInit: Failed to read %s (reason: %s)" , filename, reason);
                    free(filename);
                    ret = 1;
                    fclose( pv->file );
                    pv->file = NULL;
                    goto done;
                }
            }
            fclose( pv->file );
            pv->file = NULL;

            context->flags    |= AV_CODEC_FLAG_PASS2;
            context->stats_in  = log;
        }
        free(filename);
    }

    if (hb_avcodec_open(context, codec, &av_opts, HB_FFMPEG_THREADS_AUTO))
    {
        hb_log( "encavcodecInit: avcodec_open failed" );
        ret = 1;
        goto done;
    }

    /*
     * Reload colorimetry settings in case custom
     * values were set in the encoder_options string.
     */
    job->color_prim_override     = context->color_primaries;
    job->color_transfer_override = context->color_trc;
    job->color_matrix_override   = context->colorspace;

    if (job->pass_id == HB_PASS_ENCODE_1ST &&
        context->stats_out != NULL)
    {
        // Some encoders may write stats during init in avcodec_open
        fprintf(pv->file, "%s", context->stats_out);
    }

    // avcodec_open populates the opts dictionary with the
    // things it didn't recognize.
    AVDictionaryEntry *t = NULL;
    while( ( t = av_dict_get( av_opts, "", t, AV_DICT_IGNORE_SUFFIX ) ) )
    {
        hb_log( "encavcodecInit: Unknown avcodec option %s", t->key );
    }
    av_dict_free( &av_opts );

    pv->context = context;

    job->areBframes = 0;
    if (context->has_b_frames > 0)
    {
        if (job->vcodec == HB_VCODEC_FFMPEG_VT_H265)
        {
            // VT appears to enable b-pyramid by default and there
            // is no documented way of modifying this behaviour or
            // querying if it is enabled.
            job->areBframes = 2;
        }
        else
        {
            job->areBframes = context->has_b_frames;
        }
    }

    if (context->extradata != NULL)
    {
        memcpy(w->config->extradata.bytes, context->extradata,
                                           context->extradata_size);
        w->config->extradata.length = context->extradata_size;
    }

done:
    return ret;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
void encavcodecClose( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;

    if (pv == NULL)
    {
        return;
    }
    hb_chapter_queue_close(&pv->chapter_queue);
    if( pv->context )
    {
        hb_deep_log( 2, "encavcodec: closing libavcodec" );
        if( pv->context->codec ) {
            avcodec_flush_buffers( pv->context );
        }
        hb_avcodec_free_context(&pv->context);
    }
    if( pv->file )
    {
        fclose( pv->file );
    }
    free( pv );
    w->private_data = NULL;
}

/*
 * see comments in definition of 'frame_info' in pv struct for description
 * of what these routines are doing.
 */
static void save_frame_info( hb_work_private_t * pv, hb_buffer_t * in )
{
    int i = pv->frameno_in & FRAME_INFO_MASK;
    pv->frame_info[i].start = in->s.start;
    pv->frame_info[i].duration = in->s.stop - in->s.start;
}

static int64_t get_frame_start( hb_work_private_t * pv, int64_t frameno )
{
    int i = frameno & FRAME_INFO_MASK;
    return pv->frame_info[i].start;
}

static int64_t get_frame_duration( hb_work_private_t * pv, int64_t frameno )
{
    int i = frameno & FRAME_INFO_MASK;
    return pv->frame_info[i].duration;
}

static void compute_dts_offset( hb_work_private_t * pv, hb_buffer_t * buf )
{
    if ( pv->job->areBframes )
    {
        if ( ( pv->frameno_in ) == pv->job->areBframes )
        {
            pv->dts_delay = buf->s.start;
            pv->job->config.init_delay = pv->dts_delay;
        }
    }
}

// Generate DTS by rearranging PTS in this sequence:
// pts0 - delay, pts1 - delay, pts2 - delay, pts1, pts2, pts3...
//
// Where pts0 - ptsN are in decoded monotonically increasing presentation
// order and delay == pts1 (1 being the number of frames the decoder must
// delay before it has sufficient information to decode). The number of
// frames to delay is set by job->areBframes, so it is configurable.
// This guarantees that DTS <= PTS for any frame.
//
// This is similar to how x264 generates DTS
static hb_buffer_t * process_delay_list( hb_work_private_t * pv, hb_buffer_t * buf )
{
    if (pv->job->areBframes)
    {
        // Has dts_delay been set yet?
        hb_buffer_list_append(&pv->delay_list, buf);
        if (pv->frameno_in <= pv->job->areBframes)
        {
            // dts_delay not yet set.  queue up buffers till it is set.
            return NULL;
        }

        // We have dts_delay.  Apply it to any queued buffers renderOffset
        // and return all queued buffers.
        buf = hb_buffer_list_head(&pv->delay_list);
        while (buf != NULL)
        {
            // Use the cached frame info to get the start time of Nth frame
            // Note that start Nth frame != start time this buffer since the
            // output buffers have rearranged start times.
            if (pv->frameno_out < pv->job->areBframes)
            {
                int64_t start = get_frame_start( pv, pv->frameno_out );
                buf->s.renderOffset = start - pv->dts_delay;
            }
            else
            {
                buf->s.renderOffset = get_frame_start(pv,
                                        pv->frameno_out - pv->job->areBframes);
            }
            buf = buf->next;
            pv->frameno_out++;
        }
        buf = hb_buffer_list_clear(&pv->delay_list);
        return buf;
    }
    else if (buf != NULL)
    {
        buf->s.renderOffset = buf->s.start;
        return buf;
    }
    return NULL;
}

static void get_packets( hb_work_object_t * w, hb_buffer_list_t * list )
{
    hb_work_private_t * pv = w->private_data;

    while (1)
    {
        int           ret;
        AVPacket      pkt;
        hb_buffer_t * out;

        av_init_packet(&pkt);
        ret = avcodec_receive_packet(pv->context, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            hb_log("encavcodec: avcodec_receive_packet failed");
        }

        out = hb_buffer_init(pkt.size);
        memcpy(out->data, pkt.data, out->size);

        int64_t frameno = pkt.pts;
        out->size       = pkt.size;
        out->s.start    = get_frame_start(pv, frameno);
        out->s.duration = get_frame_duration(pv, frameno);
        out->s.stop     = out->s.stop + out->s.duration;
        // libav 12 deprecated context->coded_frame, so we can't determine
        // the exact frame type any more. So until I can completely
        // wire up ffmpeg with AV_PKT_DISPOSABLE_FRAME, all frames
        // must be considered to potentially be reference frames
        out->s.flags     = HB_FLAG_FRAMETYPE_REF;
        out->s.frametype = 0;
        if (pkt.flags & AV_PKT_FLAG_KEY)
        {
            out->s.flags |= HB_FLAG_FRAMETYPE_KEY;
            hb_chapter_dequeue(pv->chapter_queue, out);
        }
        out = process_delay_list(pv, out);

        hb_buffer_list_append(list, out);
        av_packet_unref(&pkt);
    }
}

static void Encode( hb_work_object_t *w, hb_buffer_t *in,
                    hb_buffer_list_t *list )
{
    hb_work_private_t * pv = w->private_data;
    AVFrame             frame = {0};
    int                 ret;

    frame.width       = in->f.width;
    frame.height      = in->f.height;
    frame.data[0]     = in->plane[0].data;
    frame.data[1]     = in->plane[1].data;
    frame.data[2]     = in->plane[2].data;
    frame.linesize[0] = in->plane[0].stride;
    frame.linesize[1] = in->plane[1].stride;
    frame.linesize[2] = in->plane[2].stride;

    if (in->s.new_chap > 0 && pv->job->chapter_markers)
    {
        /* chapters have to start with an IDR frame so request that this
           frame be coded as IDR. Since there may be multiple frames
           currently buffered in the encoder remember the timestamp so
           when this frame finally pops out of the encoder we'll mark
           its buffer as the start of a chapter. */
        frame.pict_type = AV_PICTURE_TYPE_I;
        frame.key_frame = 1;
        hb_chapter_enqueue(pv->chapter_queue, in);
    }

    // For constant quality, setting the quality in AVCodecContext
    // doesn't do the trick.  It must be set in the AVFrame.
    frame.quality = pv->context->global_quality;

    // Bizarro ffmpeg requires timestamp time_base to be == framerate
    // for the encoders we care about.  It writes AVCodecContext.time_base
    // to the framerate field of encoded bitstream headers, so if we
    // want correct bitstreams, we must set time_base = framerate.
    // We can't pass timestamps that are not based on the time_base
    // because encoders require accurately based timestamps in order to
    // do proper rate control.
    //
    // I.e. ffmpeg doesn't support VFR timestamps.
    //
    // Because of this, we have to do some fugly things, like storing
    // PTS values and computing DTS ourselves.
    //
    // Remember timestamp info about this frame
    save_frame_info(pv, in);
    compute_dts_offset(pv, in);

    frame.pts = pv->frameno_in++;

    // Encode
    ret = avcodec_send_frame(pv->context, &frame);
    if (ret < 0)
    {
        hb_log("encavcodec: avcodec_send_frame failed");
        return;
    }

    // Write stats
    if (pv->job->pass_id == HB_PASS_ENCODE_1ST &&
        pv->context->stats_out != NULL)
    {
        fprintf( pv->file, "%s", pv->context->stats_out );
    }

    get_packets(w, list);
}

static void Flush( hb_work_object_t * w, hb_buffer_list_t * list )
{
    hb_work_private_t * pv = w->private_data;

    avcodec_send_frame(pv->context, NULL);

    // Write stats
    // vpx only writes stats at final flush
    if (pv->job->pass_id == HB_PASS_ENCODE_1ST &&
        pv->context->stats_out != NULL)
    {
        fprintf( pv->file, "%s", pv->context->stats_out );
    }

    get_packets(w, list);
}

/***********************************************************************
 * Work
 ***********************************************************************
 *
 **********************************************************************/
int encavcodecWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                    hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t       * in = *buf_in;
    hb_buffer_list_t    list;

    if (pv->context == NULL || pv->context->codec == NULL)
    {
        hb_error("encavcodec: codec context is uninitialized");
        return HB_WORK_DONE;
    }

    hb_buffer_list_clear(&list);
    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        Flush(w, &list);
        hb_buffer_list_append(&list, hb_buffer_eof_init());
        *buf_out = hb_buffer_list_clear(&list);
        return HB_WORK_DONE;
    }

    Encode(w, in, &list);
    *buf_out = hb_buffer_list_clear(&list);

    return HB_WORK_OK;
}

static int apply_vpx_preset(AVDictionary ** av_opts, const char * preset)
{
    if (preset == NULL)
    {
        // default "medium"
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "2", 0);
    }
    else if (!strcasecmp("veryfast", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "5", 0);
    }
    else if (!strcasecmp("faster", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "4", 0);
    }
    else if (!strcasecmp("fast", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "3", 0);
    }
    else if (!strcasecmp("medium", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "2", 0);
    }
    else if (!strcasecmp("slow", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "1", 0);
    }
    else if (!strcasecmp("slower", preset))
    {
        av_dict_set( av_opts, "deadline", "good", 0);
        av_dict_set( av_opts, "cpu-used", "0", 0);
    }
    else if (!strcasecmp("veryslow", preset))
    {
        av_dict_set( av_opts, "deadline", "best", 0);
        av_dict_set( av_opts, "cpu-used", "0", 0);
    }
    else
    {
        // default "medium"
        hb_log("apply_vpx_preset: Unknown VPx encoder preset %s", preset);
        return -1;
    }

    return 0;
}

// VP8 and VP9 have some options in common and some different
static int apply_vp8_preset(AVDictionary ** av_opts, const char * preset)
{
    return apply_vpx_preset(av_opts, preset);
}

static int apply_vp9_preset(AVDictionary ** av_opts, const char * preset)
{
    av_dict_set(av_opts, "row-mt", "1", 0);
    return apply_vpx_preset(av_opts, preset);
}

static int apply_encoder_preset(int vcodec, AVDictionary ** av_opts,
                                const char * preset)
{
    switch (vcodec)
    {
        case HB_VCODEC_FFMPEG_VP8:
            return apply_vp8_preset(av_opts, preset);
        case HB_VCODEC_FFMPEG_VP9:
            return apply_vp9_preset(av_opts, preset);
        case HB_VCODEC_FFMPEG_NVENC_H264:
        case HB_VCODEC_FFMPEG_NVENC_H265:
             av_dict_set( av_opts, "preset", preset, 0);
             break;
        default:
            break;
    }
    
    return 0;
}

const char* const* hb_av_preset_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_FFMPEG_VP8:
        case HB_VCODEC_FFMPEG_VP9:
            return vpx_preset_names;

        case HB_VCODEC_FFMPEG_VCE_H264:
        case HB_VCODEC_FFMPEG_VCE_H265:
            return hb_vce_preset_names;

        case HB_VCODEC_FFMPEG_NVENC_H264:
        case HB_VCODEC_FFMPEG_NVENC_H265:
            return h26x_nvenc_preset_names;

        case HB_VCODEC_FFMPEG_VT_H264:
        case HB_VCODEC_FFMPEG_VT_H265:
            return h26x_vt_preset_name;

        default:
            return NULL;
    }
}

const char* const* hb_av_profile_get_names(int encoder)
{
    switch (encoder)
    {
        case HB_VCODEC_FFMPEG_NVENC_H264:
            return h264_nvenc_profile_names;
        case HB_VCODEC_FFMPEG_NVENC_H265:
            return h265_nvenc_profile_names;
        case HB_VCODEC_FFMPEG_VT_H264:
            return h264_vt_profile_name;
        case HB_VCODEC_FFMPEG_VT_H265:
            return h265_vt_profile_name;

         default:
             return NULL;
     }
}
