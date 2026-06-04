// ProVideoDecoder_GStreamer.cpp
#include "ProVideoDecoder.h"
#include "JuceHeader.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <mutex>

namespace
{
    static bool ensureGStreamerInitialised()
    {
        static std::once_flag once;
        static bool ok = false;
        std::call_once(once, []()
        {
            GError* err = nullptr;
            ok = gst_init_check(nullptr, nullptr, &err);
            if (err != nullptr)
                g_error_free(err);
        });
        return ok;
    }
}

struct ProVideoDecoder::Impl
{
    ~Impl()
    {
        reset();
    }

    bool ensureOpen()
    {
        if (pipeline != nullptr)
            return true;

        if (!ensureGStreamerInitialised())
            return false;

        GError* err = nullptr;
        pipeline = gst_parse_launch(
            "appsrc name=src is-live=true do-timestamp=true format=time block=false "
            "caps=video/x-h264,stream-format=byte-stream,alignment=au "
            "! h264parse disable-passthrough=true "
            "! decodebin "
            "! videoconvert "
            "! video/x-raw,format=RGB "
            "! appsink name=sink sync=false max-buffers=1 drop=true",
            &err);

        if (err != nullptr)
        {
            g_error_free(err);
            err = nullptr;
        }

        if (pipeline == nullptr)
            return false;

        appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        if (appsrc == nullptr || appsink == nullptr)
        {
            reset();
            return false;
        }

        g_object_set(appsink,
                     "emit-signals", FALSE,
                     "sync", FALSE,
                     "max-buffers", 1,
                     "drop", TRUE,
                     nullptr);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
            reset();
            return false;
        }

        return true;
    }

    bool decode(const void* data, int size, juce::Image& outImage)
    {
        if (data == nullptr || size <= 0 || !ensureOpen())
            return false;

        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, (gsize) size, nullptr);
        if (buffer == nullptr)
            return false;

        gst_buffer_fill(buffer, 0, data, (gsize) size);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frameCounter++, GST_SECOND, 30);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 30);

        const GstFlowReturn pushResult = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (pushResult != GST_FLOW_OK)
            return false;

        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 200 * GST_MSECOND);
        if (sample == nullptr)
            return false;

        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* sampleBuffer = gst_sample_get_buffer(sample);
        GstVideoInfo info;
        if (caps == nullptr || sampleBuffer == nullptr || !gst_video_info_from_caps(&info, caps))
        {
            gst_sample_unref(sample);
            return false;
        }

        GstVideoFrame frame;
        if (!gst_video_frame_map(&frame, &info, sampleBuffer, GST_MAP_READ))
        {
            gst_sample_unref(sample);
            return false;
        }

        const int width = (int) GST_VIDEO_INFO_WIDTH(&info);
        const int height = (int) GST_VIDEO_INFO_HEIGHT(&info);
        const auto* srcBase = static_cast<const unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

        juce::Image img(juce::Image::RGB, width, height, false);
        juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
        {
            const auto* src = srcBase + (size_t) y * (size_t) stride;
            for (int x = 0; x < width; ++x)
            {
                const auto* px = src + (size_t) x * 3u;
                bd.setPixelColour(x, y, juce::Colour(px[0], px[1], px[2]));
            }
        }

        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);

        if (!img.isValid())
            return false;

        outImage = img;
        return true;
    }

    void reset()
    {
        if (pipeline != nullptr)
            gst_element_set_state(pipeline, GST_STATE_NULL);

        if (appsink != nullptr)
        {
            gst_object_unref(appsink);
            appsink = nullptr;
        }

        if (appsrc != nullptr)
        {
            gst_object_unref(appsrc);
            appsrc = nullptr;
        }

        if (pipeline != nullptr)
        {
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }

        frameCounter = 0;
    }

    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    guint64 frameCounter = 0;
};

ProVideoDecoder::ProVideoDecoder()
    : impl(std::make_unique<Impl>())
{
}

ProVideoDecoder::~ProVideoDecoder() = default;

bool ProVideoDecoder::decode(const void* data, int size, juce::Image& outImage)
{
    return impl != nullptr && impl->decode(data, size, outImage);
}

void ProVideoDecoder::reset()
{
    if (impl != nullptr)
        impl->reset();
}
