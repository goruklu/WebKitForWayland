/*
 *  Copyright (C) 2009, 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *  Copyright (C) 2013 Collabora Ltd.
 *  Copyright (C) 2013 Orange
 *  Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "WebKitMediaSourceGStreamer.h"

#if ENABLE(VIDEO) && ENABLE(MEDIA_SOURCE) && USE(GSTREAMER)

#include "AudioTrackPrivateGStreamer.h"
#include "GStreamerUtilities.h"
#include "MediaDescription.h"
#include "MediaPlayerPrivateGStreamerMSE.h"
#include "MediaSample.h"
#include "MediaSourceGStreamer.h"
#include "NotImplemented.h"
#include "SourceBufferPrivateGStreamer.h"
#include "TimeRanges.h"
#include "VideoTrackPrivateGStreamer.h"

#include <gst/app/app.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/pbutils/missing-plugins.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <wtf/text/CString.h>
#include <wtf/glib/GMutexLocker.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/MainThread.h>

// To make LOG_MEDIA_MESSAGE() work outside the WebCore namespace.
using WebCore::LogMedia;

typedef struct _Stream Stream;
typedef struct _Source Source;

typedef struct {
    GstBuffer* buffer;
    WebCore::FloatSize presentationSize;
} PendingReceiveSample;

struct _Stream
{
    Source* parent;

    WebCore::StreamType type;

    // Might be 0, e.g. for VP8/VP9
    GstElement *parser;
    GstPad* srcpad;
    GstPad* demuxersrcpad;
    GstPad* multiqueuesrcpad;
    GstCaps* caps;
    gulong bufferProbeId;
    gulong bufferAfterMultiqueueProbeId;
#if ENABLE(VIDEO_TRACK)
    RefPtr<WebCore::AudioTrackPrivateGStreamer> audioTrack;
    RefPtr<WebCore::VideoTrackPrivateGStreamer> videoTrack;
#endif
    WebCore::FloatSize presentationSize;
    GList* pendingReceiveSample;
    bool initSegmentAlreadyProcessed;

    GstPad* decryptorSrcPad;
};

struct _Source {
    WebKitMediaSrc* parent;

    // AppSrc
    GstElement* src;

    // In the future there could be several Streams per Source.
    Stream* stream;

    // We expose everything when
    // all sources are noMorePads
    bool noMorePads;

    GstPad* decodebinSinkPad;

    // Just for identification
    WebCore::SourceBufferPrivateGStreamer* sourceBuffer;

    bool segmentPending;
    guint32 segmentSeqnum;
    GstSegment segment;
};

enum OnSeekDataAction {
    Nothing,
    MediaSourceSeekToTime
};

struct _WebKitMediaSrcPrivate
{
    _WebKitMediaSrcPrivate()
        : timeoutSource("[WebKit] releaseStream")
    {
        g_mutex_init(&streamMutex);
        g_cond_init(&streamCondition);
    }

    ~_WebKitMediaSrcPrivate()
    {
        g_mutex_clear(&streamMutex);
        g_cond_clear(&streamCondition);
    }

    GMutex streamMutex;
    GCond streamCondition;
    GSourceWrap::Dynamic timeoutSource;

    GList* sources;
    gchar* location;
    int nAudio;
    int nVideo;
    int nText;
    GstClockTime duration;
    bool haveAppsrc;
    bool asyncStart;
    bool allTracksConfigured;
    unsigned numberOfPads;

    MediaTime seekTime;
    GstEvent* seekEvent;

    // On seek, we wait for all the seekDatas, then for all the needDatas, and then run the nextAction.
    OnSeekDataAction appSrcSeekDataNextAction;
    int appSrcSeekDataCount;
    int appSrcNeedDataCount;

    WebCore::MediaPlayerPrivateGStreamerMSE* mediaPlayerPrivate;
    WebCore::PlaybackPipeline* mediaSourceClient;
};

enum
{
    PROP_0,
    PROP_LOCATION,
    PROP_N_AUDIO,
    PROP_N_VIDEO,
    PROP_N_TEXT,
    PROP_LAST
};

enum
{
    SIGNAL_VIDEO_CHANGED,
    SIGNAL_AUDIO_CHANGED,
    SIGNAL_TEXT_CHANGED,
    LAST_SIGNAL
};

static GstStaticPadTemplate srcTemplate = GST_STATIC_PAD_TEMPLATE("src_%u", GST_PAD_SRC,
    GST_PAD_SOMETIMES, GST_STATIC_CAPS_ANY);

#define WEBKIT_MEDIA_SRC_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), WEBKIT_TYPE_MEDIA_SRC, WebKitMediaSrcPrivate))

GST_DEBUG_CATEGORY_STATIC(webkit_media_src_debug);
#define GST_CAT_DEFAULT webkit_media_src_debug

static void webKitMediaSrcUriHandlerInit(gpointer gIface, gpointer ifaceData);
static void webKitMediaSrcFinalize(GObject*);
static void webKitMediaSrcSetProperty(GObject*, guint propertyId, const GValue*, GParamSpec*);
static void webKitMediaSrcGetProperty(GObject*, guint propertyId, GValue*, GParamSpec*);
static GstStateChangeReturn webKitMediaSrcChangeState(GstElement*, GstStateChange);
static gboolean webKitMediaSrcQueryWithParent(GstPad*, GstObject*, GstQuery*);

inline static AtomicString getStreamTrackId(Stream* stream);
static GstClockTime floatToGstClockTime(float);

static void app_src_need_data (GstAppSrc *src, guint length, gpointer user_data);
static void app_src_enough_data (GstAppSrc *src, gpointer user_data);
static gboolean app_src_seek_data (GstAppSrc *src, guint64 offset, gpointer user_data);

static GstAppSrcCallbacks appsrcCallbacks = {
    app_src_need_data,
    app_src_enough_data,
    app_src_seek_data,
    { 0 }
};

#define webkit_media_src_parent_class parent_class
// We split this out into another macro to avoid a check-webkit-style error.
#define WEBKIT_MEDIA_SRC_CATEGORY_INIT GST_DEBUG_CATEGORY_INIT(webkit_media_src_debug, "webkitmediasrc", 0, "websrc element");
G_DEFINE_TYPE_WITH_CODE(WebKitMediaSrc, webkit_media_src, GST_TYPE_BIN,
    G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER, webKitMediaSrcUriHandlerInit);
    WEBKIT_MEDIA_SRC_CATEGORY_INIT);

static guint webkit_media_src_signals[LAST_SIGNAL] = { 0 };

static void webkit_media_src_class_init(WebKitMediaSrcClass* klass)
{
    GObjectClass* oklass = G_OBJECT_CLASS(klass);
    GstElementClass* eklass = GST_ELEMENT_CLASS(klass);

    oklass->finalize = webKitMediaSrcFinalize;
    oklass->set_property = webKitMediaSrcSetProperty;
    oklass->get_property = webKitMediaSrcGetProperty;

    gst_element_class_add_pad_template(eklass, gst_static_pad_template_get(&srcTemplate));

    gst_element_class_set_static_metadata(eklass, "WebKit Media source element", "Source", "Handles Blob uris", "Stephane Jadaud <sjadaud@sii.fr>, Sebastian Dröge <sebastian@centricular.com>");

    /* Allows setting the uri using the 'location' property, which is used
     * for example by gst_element_make_from_uri() */
    g_object_class_install_property(oklass,
        PROP_LOCATION,
        g_param_spec_string("location", "location", "Location to read from", 0,
        (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property (oklass,
        PROP_N_AUDIO,
        g_param_spec_int ("n-audio", "Number Audio", "Total number of audio streams",
        0, G_MAXINT, 0, (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property (oklass,
        PROP_N_VIDEO,
        g_param_spec_int ("n-video", "Number Video", "Total number of video streams",
        0, G_MAXINT, 0, (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property (oklass,
        PROP_N_TEXT,
        g_param_spec_int ("n-text", "Number Text", "Total number of text streams",
        0, G_MAXINT, 0, (GParamFlags) (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

    webkit_media_src_signals[SIGNAL_VIDEO_CHANGED] =
        g_signal_new ("video-changed", G_TYPE_FROM_CLASS (oklass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (WebKitMediaSrcClass, video_changed), NULL, NULL,
        g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);
    webkit_media_src_signals[SIGNAL_AUDIO_CHANGED] =
        g_signal_new ("audio-changed", G_TYPE_FROM_CLASS (oklass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (WebKitMediaSrcClass, audio_changed), NULL, NULL,
        g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);
    webkit_media_src_signals[SIGNAL_TEXT_CHANGED] =
        g_signal_new ("text-changed", G_TYPE_FROM_CLASS (oklass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (WebKitMediaSrcClass, text_changed), NULL, NULL,
        g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

    eklass->change_state = webKitMediaSrcChangeState;

    g_type_class_add_private(klass, sizeof(WebKitMediaSrcPrivate));
}

static void webkit_media_src_init(WebKitMediaSrc* src)
{
    src->priv = WEBKIT_MEDIA_SRC_GET_PRIVATE(src);
    new (src->priv) WebKitMediaSrcPrivate();
    src->priv->seekTime = MediaTime::invalidTime();
    src->priv->appSrcSeekDataCount = 0;
    src->priv->appSrcNeedDataCount = 0;
    src->priv->appSrcSeekDataNextAction = Nothing;
}

static void webKitMediaSrcFinalize(GObject* object)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(object);
    WebKitMediaSrcPrivate* priv = src->priv;

    // TODO: Free sources
    g_free(priv->location);

    priv->seekTime = MediaTime::invalidTime();

    if (priv->seekEvent) {
        gst_event_unref(priv->seekEvent);
        priv->seekEvent = 0;
    }

    if (priv->mediaPlayerPrivate)
        priv->mediaPlayerPrivate = 0;

    GST_CALL_PARENT(G_OBJECT_CLASS, finalize, (object));
}

static void webKitMediaSrcSetProperty(GObject* object, guint propId, const GValue* value, GParamSpec* pspec)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(object);

    switch (propId) {
    case PROP_LOCATION:
        gst_uri_handler_set_uri(reinterpret_cast<GstURIHandler*>(src), g_value_get_string(value), 0);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propId, pspec);
        break;
    }
}

static void webKitMediaSrcGetProperty(GObject* object, guint propId, GValue* value, GParamSpec* pspec)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(object);
    WebKitMediaSrcPrivate* priv = src->priv;

    GST_OBJECT_LOCK(src);
    switch (propId) {
    case PROP_LOCATION:
        g_value_set_string(value, priv->location);
        break;
    case PROP_N_AUDIO:
        g_value_set_int(value, priv->nAudio);
        break;
    case PROP_N_VIDEO:
        g_value_set_int(value, priv->nVideo);
        break;
    case PROP_N_TEXT:
        g_value_set_int(value, priv->nText);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propId, pspec);
        break;
    }
    GST_OBJECT_UNLOCK(src);
}

static void webKitMediaSrcDoAsyncStart(WebKitMediaSrc* src)
{
    WebKitMediaSrcPrivate* priv = src->priv;
    priv->asyncStart = true;
    GST_BIN_CLASS(parent_class)->handle_message(GST_BIN(src),
        gst_message_new_async_start(GST_OBJECT(src)));
}

static void webKitMediaSrcDoAsyncDone(WebKitMediaSrc* src)
{
    WebKitMediaSrcPrivate* priv = src->priv;
    if (priv->asyncStart) {
        GST_BIN_CLASS(parent_class)->handle_message(GST_BIN(src),
            gst_message_new_async_done(GST_OBJECT(src), GST_CLOCK_TIME_NONE));
        priv->asyncStart = false;
    }
}

static GstStateChangeReturn webKitMediaSrcChangeState(GstElement* element, GstStateChange transition)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(element);
    WebKitMediaSrcPrivate* priv = src->priv;

    switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
        priv->allTracksConfigured = false;
        webKitMediaSrcDoAsyncStart(src);
        break;
    default:
        break;
    }

    ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
    if (G_UNLIKELY(ret == GST_STATE_CHANGE_FAILURE)) {
        GST_DEBUG_OBJECT(src, "State change failed");
        webKitMediaSrcDoAsyncDone(src);
        return ret;
    }

    switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
        ret = GST_STATE_CHANGE_ASYNC;
        break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
        webKitMediaSrcDoAsyncDone(src);
        priv->allTracksConfigured = false;
        break;
    default:
        break;
    }

    return ret;
}

static gboolean webKitMediaSrcQueryWithParent(GstPad* pad, GstObject* parent, GstQuery* query)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(GST_ELEMENT(parent));
    gboolean result = FALSE;

    switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_DURATION: {
        GstFormat format;
        gst_query_parse_duration(query, &format, NULL);

        GST_DEBUG_OBJECT(src, "duration query in format %s", gst_format_get_name(format));
        GST_OBJECT_LOCK(src);
        if ((format == GST_FORMAT_TIME) && src && src->priv && (src->priv->duration > 0)) {
            gst_query_set_duration(query, format, src->priv->duration);
            result = TRUE;
        }
        GST_OBJECT_UNLOCK(src);
        break;
    }
    case GST_QUERY_URI:
        GST_OBJECT_LOCK(src);
        if (src && src->priv)
            gst_query_set_uri(query, src->priv->location);
        GST_OBJECT_UNLOCK(src);
        result = TRUE;
        break;
    default:{
        GRefPtr<GstPad> target = adoptGRef(gst_ghost_pad_get_target(GST_GHOST_PAD_CAST(pad)));
        // Forward the query to the proxy target pad.
        if (target)
            result = gst_pad_query(target.get(), query);
        break;
    }
    }

    return result;
}

static void webKitMediaSrcCheckAllTracksConfigured(WebKitMediaSrc* webKitMediaSrc);

static void webKitMediaSrcUpdatePresentationSize(GstCaps* caps, Stream* stream)
{
    GstStructure* s = gst_caps_get_structure(caps, 0);
    const gchar* structureName = gst_structure_get_name(s);
    GstVideoInfo info;
    bool sizeConfigured = false;

#if GST_CHECK_VERSION(1, 5, 3)
    if (gst_structure_has_name(s, "application/x-cenc")) {
        const gchar* originalMediaType = gst_structure_get_string(s, "original-media-type");
        if (g_str_has_prefix(originalMediaType, "video/")) {
            int width = 0;
            int height = 0;
            float finalHeight = 0;

            gst_structure_get_int(s, "width", &width);
            if (gst_structure_get_int(s, "height", &height)) {
                gint par_n = 1;
                gint par_d = 1;

                gst_structure_get_fraction(s, "pixel-aspect-ratio", &par_n, &par_d);
                finalHeight = height * ((float) par_d / (float) par_n);
            }

            GST_OBJECT_LOCK(stream->parent->parent);
            stream->presentationSize = WebCore::FloatSize(width, finalHeight);
            GST_OBJECT_UNLOCK(stream->parent->parent);
        } else {
            GST_OBJECT_LOCK(stream->parent->parent);
            stream->presentationSize = WebCore::FloatSize();
            GST_OBJECT_UNLOCK(stream->parent->parent);
        }
        sizeConfigured = true;
    }
#endif

    if (!sizeConfigured) {
        if (g_str_has_prefix(structureName, "video/") && gst_video_info_from_caps(&info, caps)) {
            float width, height;

            // TODO: correct?
            width = info.width;
            height = info.height * ((float) info.par_d / (float) info.par_n);

            GST_OBJECT_LOCK(stream->parent->parent);
            stream->presentationSize = WebCore::FloatSize(width, height);
            GST_OBJECT_UNLOCK(stream->parent->parent);
        } else {
            GST_OBJECT_LOCK(stream->parent->parent);
            stream->presentationSize = WebCore::FloatSize();
            GST_OBJECT_UNLOCK(stream->parent->parent);
        }
    }

    gst_caps_ref(caps);
    GST_OBJECT_LOCK(stream->parent->parent);
    if (stream->caps)
        gst_caps_unref(stream->caps);

    stream->caps = caps;
    GST_OBJECT_UNLOCK(stream->parent->parent);
}

static void webKitMediaSrcLinkStreamToSrcPad(GstPad* srcpad, Stream* stream)
{
    Source* source = stream->parent;

    unsigned padId = static_cast<unsigned>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(srcpad), "id")));
    GST_DEBUG_OBJECT(source->parent, "linking stream to src pad (id: %u)", padId);

    gchar* padName = g_strdup_printf("src_%u", padId);
    GstPad* ghostpad = gst_ghost_pad_new_from_template(padName, srcpad, gst_static_pad_template_get(&srcTemplate));

    gst_pad_set_query_function(ghostpad, webKitMediaSrcQueryWithParent);

    gst_pad_set_element_private(ghostpad, stream);

    gst_pad_set_active(ghostpad, TRUE);
    gst_element_add_pad(GST_ELEMENT(source->parent), ghostpad);

    GST_OBJECT_LOCK(stream->parent->parent);
    stream->srcpad = ghostpad;
    GST_OBJECT_UNLOCK(stream->parent->parent);

    if (source->decodebinSinkPad) {
        GST_DEBUG_OBJECT(source->parent, "A decodebin was previously used for this source, trying to reuse it.");
        // TODO: error checking here. Not sure what to do if linking
        // fails though, because decodebin is out of this src
        // element's scope, in theory.
        gst_pad_link(ghostpad, source->decodebinSinkPad);
    }
}

static void webKitMediaSrcParserNotifyCaps(GObject* object, GParamSpec*, Stream* stream)
{
    GstPad* srcpad = GST_PAD(object);
    GstCaps* caps = gst_pad_get_current_caps(srcpad);

    if (!caps || !stream->parent) {
        return;
    }

    LOG_MEDIA_MESSAGE("Caps changed");

    webKitMediaSrcUpdatePresentationSize(caps, stream);
    gst_caps_unref(caps);

    // TODO
    if (!gst_pad_is_linked(srcpad)) {
        GST_DEBUG_OBJECT(stream->parent, "pad not linked yet");
        webKitMediaSrcLinkStreamToSrcPad(srcpad, stream);
    }

    webKitMediaSrcCheckAllTracksConfigured(stream->parent->parent);
}

static gboolean releaseStream(WebKitMediaSrc* src, Stream* stream)
{
    GST_DEBUG("Freeing stream %p", stream);

    WTF::GMutexLocker<GMutex> lock(src->priv->streamMutex);

    if (stream->caps)
        gst_caps_unref(stream->caps);
#if ENABLE(VIDEO_TRACK)
    if (stream->audioTrack) {
        stream->audioTrack = nullptr;
    }
    if (stream->videoTrack) {
        stream->videoTrack = nullptr;
    }
#endif
    if (stream->multiqueuesrcpad)
        gst_object_unref(stream->multiqueuesrcpad);

    if (stream->decryptorSrcPad)
        gst_object_unref(stream->decryptorSrcPad);

    if (stream->pendingReceiveSample) {
        for (GList* l = stream->pendingReceiveSample; l; l = l->next) {
            PendingReceiveSample* receiveSample = static_cast<PendingReceiveSample*>(l->data);
            gst_buffer_unref(receiveSample->buffer);
        }

        g_list_free(stream->pendingReceiveSample);
    }

    int signal = -1;
    switch (stream->type) {
    case WebCore::Audio:
        signal = SIGNAL_AUDIO_CHANGED;
        break;
    case WebCore::Video:
        signal = SIGNAL_VIDEO_CHANGED;
        break;
    case WebCore::Text:
        signal = SIGNAL_TEXT_CHANGED;
        break;
    default:
        break;
    }

    // FIXME: enable this when the track removal no longer crashes.
    if (signal != -1)
        g_signal_emit(G_OBJECT(src), webkit_media_src_signals[signal], 0, NULL);

    g_cond_signal(&src->priv->streamCondition);
    return G_SOURCE_REMOVE;
}

static gboolean freeSourceLater(Source* source)
{
    GST_DEBUG("Releasing source: %p", source);
    g_free(source);

    return G_SOURCE_REMOVE;
}

static void webKitMediaSrcCheckAllTracksConfigured(WebKitMediaSrc* webKitMediaSrc)
{
    bool allTracksConfigured = false;

    GST_OBJECT_LOCK(webKitMediaSrc);
    if (!(webKitMediaSrc->priv->allTracksConfigured)) {
        allTracksConfigured = true;
        for (GList* sources = webKitMediaSrc->priv->sources; sources; sources = sources->next) {
            Source* s = static_cast<Source*>(sources->data);
            if (!s->stream) {
                allTracksConfigured = false;
                break;
            }
        }
        if (allTracksConfigured)
            webKitMediaSrc->priv->allTracksConfigured = true;
    }
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    if (allTracksConfigured) {
        LOG_MEDIA_MESSAGE("All tracks attached. Completing async state change operation.");
        gst_element_no_more_pads(GST_ELEMENT(webKitMediaSrc));
        webKitMediaSrcDoAsyncDone(webKitMediaSrc);
    }
}

// uri handler interface
static GstURIType webKitMediaSrcUriGetType(GType)
{
    return GST_URI_SRC;
}

const gchar* const* webKitMediaSrcGetProtocols(GType)
{
    static const char* protocols[] = {"mediasourceblob", 0 };
    return protocols;
}

static gchar* webKitMediaSrcGetUri(GstURIHandler* handler)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(handler);
    gchar* ret;

    GST_OBJECT_LOCK(src);
    ret = g_strdup(src->priv->location);
    GST_OBJECT_UNLOCK(src);
    return ret;
}

static gboolean webKitMediaSrcSetUri(GstURIHandler* handler, const gchar* uri, GError**)
{
    WebKitMediaSrc* src = WEBKIT_MEDIA_SRC(handler);
    WebKitMediaSrcPrivate* priv = src->priv;

    if (GST_STATE(src) >= GST_STATE_PAUSED) {
        GST_ERROR_OBJECT(src, "URI can only be set in states < PAUSED");
        return FALSE;
    }

    GST_OBJECT_LOCK(src);
    g_free(priv->location);
    priv->location = 0;
    if (!uri) {
        GST_OBJECT_UNLOCK(src);
        return TRUE;
    }

    WebCore::URL url(WebCore::URL(), uri);

    priv->location = g_strdup(url.string().utf8().data());
    GST_OBJECT_UNLOCK(src);
    return TRUE;
}
static void webKitMediaSrcUriHandlerInit(gpointer gIface, gpointer)
{
    GstURIHandlerInterface* iface = (GstURIHandlerInterface *) gIface;

    iface->get_type = webKitMediaSrcUriGetType;
    iface->get_protocols = webKitMediaSrcGetProtocols;
    iface->get_uri = webKitMediaSrcGetUri;
    iface->set_uri = webKitMediaSrcSetUri;
}

inline static AtomicString getStreamTrackId(Stream* stream)
{
    if (stream->audioTrack)
        return stream->audioTrack->id();
    if (stream->videoTrack)
        return stream->videoTrack->id();
    GST_DEBUG("Stream has no audio and no video track");
    return AtomicString();
}

static Stream* getStreamByTrackId(WebKitMediaSrc* src, AtomicString trackIDString)
{
    // WebKitMediaSrc should be locked at this point.
    for (GList* sources = src->priv->sources; sources; sources = sources->next) {
        Source* source = static_cast<Source*>(sources->data);
        if (source->stream && (
            (source->stream->audioTrack && source->stream->audioTrack->id() == trackIDString) ||
            (source->stream->videoTrack && source->stream->videoTrack->id() == trackIDString) ) )
            return source->stream;
    }
    return NULL;
}

static Source* getSourceBySourceBufferPrivate(WebKitMediaSrc* src, WebCore::SourceBufferPrivateGStreamer* sourceBufferPrivate)
{
    for (GList* sources = src->priv->sources; sources; sources = sources->next) {
        Source* source = static_cast<Source*>(sources->data);
        if (source->sourceBuffer == sourceBufferPrivate)
            return source;
    }
    return NULL;
}

static gboolean seekNeedsDataMainThread (gpointer user_data)
{
    WebKitMediaSrc* webKitMediaSrc = static_cast<WebKitMediaSrc*>(user_data);
    g_assert(WEBKIT_IS_MEDIA_SRC(webKitMediaSrc));

    LOG_MEDIA_MESSAGE("%s", "");

    g_assert(WTF::isMainThread());

    GST_OBJECT_LOCK(webKitMediaSrc);
    MediaTime seekTime = webKitMediaSrc->priv->seekTime;
    RefPtr<WebCore::MediaPlayerPrivateGStreamerMSE> mediaPlayerPrivate(webKitMediaSrc->priv->mediaPlayerPrivate);
    GST_OBJECT_UNLOCK(webKitMediaSrc);
    mediaPlayerPrivate->notifySeekNeedsData(seekTime);

    return G_SOURCE_REMOVE;
}

static void app_src_need_data (GstAppSrc *src, guint length, gpointer user_data)
{
    UNUSED_PARAM(src);
    UNUSED_PARAM(length);

    WebKitMediaSrc* webKitMediaSrc = static_cast<WebKitMediaSrc*>(user_data);
    g_assert(WEBKIT_IS_MEDIA_SRC(webKitMediaSrc));

    OnSeekDataAction appSrcSeekDataNextAction;
    bool allAppSrcsNeedDataAfterSeek = false;

    GST_OBJECT_LOCK(webKitMediaSrc);
    int numAppSrcs = g_list_length(webKitMediaSrc->priv->sources);

    if (webKitMediaSrc->priv->appSrcSeekDataCount > 0) {
        ++webKitMediaSrc->priv->appSrcNeedDataCount;
        if (webKitMediaSrc->priv->appSrcSeekDataCount == numAppSrcs && webKitMediaSrc->priv->appSrcNeedDataCount == numAppSrcs) {
            LOG_MEDIA_MESSAGE("All need_datas completed");
            allAppSrcsNeedDataAfterSeek = true;
            appSrcSeekDataNextAction = webKitMediaSrc->priv->appSrcSeekDataNextAction;
            webKitMediaSrc->priv->appSrcSeekDataCount = 0;
            webKitMediaSrc->priv->appSrcNeedDataCount = 0;
            webKitMediaSrc->priv->appSrcSeekDataNextAction = Nothing;
        }
    }
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    if (allAppSrcsNeedDataAfterSeek) {
        LOG_MEDIA_MESSAGE("All expected app_src_seek_data() and app_src_need_data() calls performed. Running next action (%d)", static_cast<int>(appSrcSeekDataNextAction));

        switch (appSrcSeekDataNextAction) {
        case MediaSourceSeekToTime:
            if (WTF::isMainThread())
                seekNeedsDataMainThread(user_data);
            else
                g_timeout_add(0, GSourceFunc(seekNeedsDataMainThread), user_data);
            break;
        case Nothing:
            break;
        }
    }
}

static void app_src_enough_data (GstAppSrc *src, gpointer user_data)
{
    UNUSED_PARAM(src);
    UNUSED_PARAM(user_data);
}

static gboolean app_src_seek_data (GstAppSrc *src, guint64 offset, gpointer user_data)
{
    UNUSED_PARAM(src);
    UNUSED_PARAM(offset);

    g_assert(WTF::isMainThread());

    WebKitMediaSrc* webKitMediaSrc = static_cast<WebKitMediaSrc*>(user_data);

    g_assert(WEBKIT_IS_MEDIA_SRC(webKitMediaSrc));

    GST_OBJECT_LOCK(webKitMediaSrc);
    webKitMediaSrc->priv->appSrcSeekDataCount++;
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    return TRUE;
}

namespace WebCore {

// ########### TODO: Use MediaSourceClientGStreamerMSE

PassRefPtr<PlaybackPipeline> PlaybackPipeline::create()
{
    return adoptRef(new PlaybackPipeline());
}

PlaybackPipeline::PlaybackPipeline()
    : RefCounted<PlaybackPipeline>()
{
}

PlaybackPipeline::~PlaybackPipeline()
{
}

void PlaybackPipeline::setWebKitMediaSrc(WebKitMediaSrc* webKitMediaSrc)
{
    LOG_MEDIA_MESSAGE("webKitMediaSrc=%p", webKitMediaSrc);
    m_webKitMediaSrc = adoptGRef(static_cast<WebKitMediaSrc*>(gst_object_ref(webKitMediaSrc)));
    m_webKitMediaSrc->priv->mediaSourceClient = this;
}

WebKitMediaSrc* PlaybackPipeline::webKitMediaSrc()
{
    return m_webKitMediaSrc.get();
}

MediaSourcePrivate::AddStatus PlaybackPipeline::addSourceBuffer(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;

    if (priv->allTracksConfigured) {
        GST_ERROR_OBJECT(m_webKitMediaSrc.get(), "Adding new source buffers after first data not supported yet");
        return MediaSourcePrivate::NotSupported;
    }

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "State %d", (int)GST_STATE(m_webKitMediaSrc.get()));

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    guint numberOfSources = g_list_length(priv->sources);
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    Source* source = g_new0(Source, 1);
    GUniquePtr<gchar> srcName(g_strdup_printf("src%u", numberOfSources));
    GUniquePtr<gchar> typefindName(g_strdup_printf("typefind%u", numberOfSources));
    source->parent = m_webKitMediaSrc.get();
    source->src = gst_element_factory_make("appsrc", srcName.get());
    gst_app_src_set_callbacks(GST_APP_SRC(source->src), &appsrcCallbacks, source->parent, 0);
    gst_app_src_set_emit_signals(GST_APP_SRC(source->src), FALSE);
    gst_app_src_set_stream_type(GST_APP_SRC(source->src), GST_APP_STREAM_TYPE_SEEKABLE);

    source->sourceBuffer = sourceBufferPrivate.get();

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    priv->sources = g_list_prepend(priv->sources, source);
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    gst_bin_add(GST_BIN(m_webKitMediaSrc.get()), source->src);
    gst_element_sync_state_with_parent(source->src);

    return MediaSourcePrivate::Ok;
}

void PlaybackPipeline::removeSourceBuffer(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Element removed from MediaSource");
    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;
    Source* source = 0;
    GList *l;

    for (l = priv->sources; l; l = l->next) {
        Source *tmp = static_cast<Source*>(l->data);
        if (tmp->sourceBuffer == sourceBufferPrivate.get()) {
            source = tmp;
            break;
        }
    }
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    if (source) {
        if (source->src)
            gst_app_src_end_of_stream(GST_APP_SRC(source->src));

        if (source->stream) {
            if (WTF::isMainThread())
                releaseStream(source->parent, source->stream);
            else {
                WTF::GMutexLocker<GMutex> lock(source->parent->priv->streamMutex);
                WebCore::GstObjectRef protector(GST_OBJECT(source->parent));
                Stream* stream = source->stream;
                source->parent->priv->timeoutSource.schedule([protector, stream] { releaseStream(WEBKIT_MEDIA_SRC(protector.get()), stream); });

                g_cond_wait(&source->parent->priv->streamCondition, &source->parent->priv->streamMutex);
            }
            source->stream = 0;
        }

        g_timeout_add(300, (GSourceFunc)freeSourceLater, source);
    }
}

void PlaybackPipeline::attachTrack(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, RefPtr<TrackPrivateBase> trackPrivate, GstCaps* caps)
{
    LOG_MEDIA_MESSAGE("%s", "");

    WebKitMediaSrc* webKitMediaSrc = m_webKitMediaSrc.get();
    Source* source = 0;
    GstCaps* appsrccaps = 0;
    GstStructure* s = 0;
    const gchar* appsrctypename = 0;
    const gchar* mediaType = 0;

    Stream* stream = g_new0(Stream, 1);
    gchar *parserBinName;
    bool capsNotifyHandlerConnected = false;
    unsigned padId = 0;

    GST_OBJECT_LOCK(webKitMediaSrc);
    source = getSourceBySourceBufferPrivate(webKitMediaSrc, sourceBufferPrivate.get());
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    g_assert(source != 0);

    gst_app_src_set_caps(GST_APP_SRC(source->src), caps);
    appsrccaps = gst_app_src_get_caps(GST_APP_SRC(source->src));
    s = gst_caps_get_structure(appsrccaps, 0);
    appsrctypename = gst_structure_get_name(s);
    mediaType = appsrctypename;

#if GST_CHECK_VERSION(1, 5, 3)
    GstElement* decryptor = nullptr;
    if (gst_structure_has_name(s, "application/x-cenc"))
        mediaType = gst_structure_get_string(s, "original-media-type");
#endif

    stream->decryptorSrcPad = nullptr;

    GST_OBJECT_LOCK(webKitMediaSrc);
    padId = source->parent->priv->numberOfPads;
    source->parent->priv->numberOfPads++;
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    stream->parent = source;
    stream->initSegmentAlreadyProcessed = false;
    stream->type = Unknown;

    parserBinName = g_strdup_printf("streamparser%u", padId);

    g_assert(appsrccaps != 0);

    stream->parser = gst_bin_new(parserBinName);
    g_free(parserBinName);

    GST_DEBUG_OBJECT(webKitMediaSrc, "Configured track %s: appsrc=%s, padId=%u, caps=%" GST_PTR_FORMAT, trackPrivate->id().string().utf8().data(), GST_ELEMENT_NAME(source->src), padId, appsrccaps);

#if GST_CHECK_VERSION(1, 5, 3)
    if (gst_structure_has_name(s, "application/x-cenc")) {
        decryptor = WebCore::createGstDecryptor(gst_structure_get_string(s, "protection-system"));
        if (!decryptor) {
            GST_ERROR_OBJECT(webKitMediaSrc, "decryptor not found for caps: %" GST_PTR_FORMAT, appsrccaps);
            gst_object_unref(GST_OBJECT(stream->parser));
            return;
        }

        stream->decryptorSrcPad = gst_element_get_static_pad(decryptor, "src");
        g_signal_connect(stream->decryptorSrcPad, "notify::caps", G_CALLBACK(webKitMediaSrcParserNotifyCaps), stream);
        capsNotifyHandlerConnected = true;
        gst_bin_add(GST_BIN(stream->parser), decryptor);
    }
#endif

    if (!g_strcmp0(mediaType, "video/x-h264")) {
        GstElement* parser;
        GstElement* capsfilter;
        GstPad* pad = nullptr;
        GstCaps* filtercaps;

        filtercaps = gst_caps_new_simple("video/x-h264", "alignment", G_TYPE_STRING, "au", NULL);
        parser = gst_element_factory_make("h264parse", 0);
        capsfilter = gst_element_factory_make("capsfilter", 0);
        g_object_set(capsfilter, "caps", filtercaps, NULL);
        gst_caps_unref(filtercaps);

        gst_bin_add_many(GST_BIN(stream->parser), parser, capsfilter, NULL);
#if GST_CHECK_VERSION(1, 5, 3)
        if (decryptor) {
            gst_element_link_pads(decryptor, "src", parser, "sink");
            pad = gst_element_get_static_pad(decryptor, "sink");
        }
#endif
        gst_element_link_pads(parser, "src", capsfilter, "sink");

        if (!pad)
            pad = gst_element_get_static_pad(parser, "sink");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("sink", pad));
        gst_object_unref(pad);

        pad = gst_element_get_static_pad(capsfilter, "src");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
    } else if (!g_strcmp0(mediaType, "video/x-h265")) {
        GstElement* parser;
        GstElement* capsfilter;
        GstPad* pad = nullptr;
        GstCaps* filtercaps;

        filtercaps = gst_caps_new_simple("video/x-h265", "alignment", G_TYPE_STRING, "au", NULL);
        parser = gst_element_factory_make("h265parse", 0);
        capsfilter = gst_element_factory_make("capsfilter", 0);
        g_object_set(capsfilter, "caps", filtercaps, NULL);
        gst_caps_unref(filtercaps);

        gst_bin_add_many(GST_BIN(stream->parser), parser, capsfilter, NULL);

#if GST_CHECK_VERSION(1, 5, 3)
        if (decryptor) {
            gst_element_link_pads(decryptor, "src", parser, "sink");
            pad = gst_element_get_static_pad(decryptor, "sink");
        }
#endif
        gst_element_link_pads(parser, "src", capsfilter, "sink");

        if (!pad)
            pad = gst_element_get_static_pad(parser, "sink");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("sink", pad));
        gst_object_unref(pad);

        pad = gst_element_get_static_pad(capsfilter, "src");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
    } else if (!g_strcmp0(mediaType, "audio/mpeg")) {
        gint mpegversion = -1;
        GstElement* parser;
        GstPad* pad = nullptr;

        gst_structure_get_int(s, "mpegversion", &mpegversion);
        if (mpegversion == 1) {
            parser = gst_element_factory_make("mpegaudioparse", 0);
        } else if (mpegversion == 2 || mpegversion == 4) {
            parser = gst_element_factory_make("aacparse", 0);
        } else {
            g_assert_not_reached();
        }

        gst_bin_add(GST_BIN(stream->parser), parser);

#if GST_CHECK_VERSION(1, 5, 3)
        if (decryptor) {
            gst_element_link_pads(decryptor, "src", parser, "sink");
            pad = gst_element_get_static_pad(decryptor, "sink");
        }
#endif

        if (!pad)
            pad = gst_element_get_static_pad(parser, "sink");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("sink", pad));
        gst_object_unref(pad);

        pad = gst_element_get_static_pad(parser, "src");
        gst_element_add_pad(stream->parser, gst_ghost_pad_new("src", pad));
        gst_object_unref(pad);
    } else {
        GST_ERROR_OBJECT(source->parent, "Unsupported caps: %" GST_PTR_FORMAT, appsrccaps);
        gst_object_unref(GST_OBJECT(stream->parser));
        return;
    }

    gst_caps_unref(appsrccaps);

    GST_OBJECT_LOCK(webKitMediaSrc);
    if (source->stream) {
        LOG_MEDIA_MESSAGE("source->stream already has a value, use reattachTrack() instead");
        g_assert_null(source->stream);
    }
    source->stream = stream;
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    ASSERT(stream->parser);
    gst_bin_add(GST_BIN(source->parent), stream->parser);
    gst_element_sync_state_with_parent(stream->parser);

    GstPad* sinkpad = gst_element_get_static_pad(stream->parser, "sink");
    GstPad* srcpad = gst_element_get_static_pad(source->src, "src");
    gst_pad_link(srcpad, sinkpad);
    gst_object_unref(srcpad);
    srcpad = 0;
    gst_object_unref(sinkpad);
    sinkpad = 0;

    srcpad = gst_element_get_static_pad(stream->parser, "src");
    // TODO: Is padId the best way to identify the Source-Stream? What about trackId?
    g_object_set_data(G_OBJECT(srcpad), "id", GINT_TO_POINTER(padId));
    if (!capsNotifyHandlerConnected)
        g_signal_connect(srcpad, "notify::caps", G_CALLBACK(webKitMediaSrcParserNotifyCaps), stream);
    webKitMediaSrcLinkStreamToSrcPad(srcpad, stream);

    ASSERT(source->parent->priv->mediaPlayerPrivate);
    int signal = -1;
    if (g_str_has_prefix(mediaType, "audio")) {
        GST_OBJECT_LOCK(webKitMediaSrc);
        stream->type = Audio;
        source->parent->priv->nAudio++;
        GST_OBJECT_UNLOCK(webKitMediaSrc);
        signal = SIGNAL_AUDIO_CHANGED;

        stream->audioTrack = RefPtr<WebCore::AudioTrackPrivateGStreamer>(static_cast<WebCore::AudioTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (g_str_has_prefix(mediaType, "video")) {
        GST_OBJECT_LOCK(webKitMediaSrc);
        stream->type = Video;
        source->parent->priv->nVideo++;
        GST_OBJECT_UNLOCK(webKitMediaSrc);
        signal = SIGNAL_VIDEO_CHANGED;

        stream->videoTrack = RefPtr<WebCore::VideoTrackPrivateGStreamer>(static_cast<WebCore::VideoTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (g_str_has_prefix(mediaType, "text")) {
        GST_OBJECT_LOCK(webKitMediaSrc);
        stream->type = Text;
        source->parent->priv->nText++;
        GST_OBJECT_UNLOCK(webKitMediaSrc);
        signal = SIGNAL_TEXT_CHANGED;

        // TODO: Support text tracks.
    }

    if (signal != -1)
        g_signal_emit(G_OBJECT(source->parent), webkit_media_src_signals[signal], 0, NULL);

    gst_object_unref(srcpad);
    srcpad = 0;
}

void PlaybackPipeline::reattachTrack(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, RefPtr<TrackPrivateBase> trackPrivate, GstCaps* caps)
{
    LOG_MEDIA_MESSAGE("%s", "");

    UNUSED_PARAM(caps);

    // TODO: Maybe remove this method.
    // Now the caps change is managed by gst_appsrc_push_sample()
    // in enqueueSample() and flushAndEnqueueNonDisplayingSamples().

    WebKitMediaSrc* webKitMediaSrc = m_webKitMediaSrc.get();

    GST_OBJECT_LOCK(webKitMediaSrc);
    Source* source = getSourceBySourceBufferPrivate(webKitMediaSrc, sourceBufferPrivate.get());
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    g_assert(source != 0);

    Stream* stream = source->stream;

    g_assert(stream != 0);

    GstCaps* oldAppsrccaps = gst_app_src_get_caps(GST_APP_SRC(source->src));
    // Now the caps change is managed by gst_appsrc_push_sample()
    // in enqueueSample() and flushAndEnqueueNonDisplayingSamples().
    // gst_app_src_set_caps(GST_APP_SRC(source->src), caps);
    GstCaps* appsrccaps = gst_app_src_get_caps(GST_APP_SRC(source->src));
    const gchar* mediaType = gst_structure_get_name(gst_caps_get_structure(appsrccaps, 0));

    if (!gst_caps_is_equal(oldAppsrccaps, appsrccaps)) {
        LOG_MEDIA_MESSAGE("Caps have changed, but reconstructing the sequence of elements is not supported yet");

        gchar* stroldcaps = gst_caps_to_string(oldAppsrccaps);
        gchar* strnewcaps = gst_caps_to_string(appsrccaps);
        LOG_MEDIA_MESSAGE("oldcaps: %s\nnewcaps: %s", stroldcaps, strnewcaps);
        g_free(stroldcaps);
        g_free(strnewcaps);
    }

    int signal = -1;

    GST_OBJECT_LOCK(webKitMediaSrc);
    if (g_str_has_prefix(mediaType, "audio")) {
        g_assert(stream->type == Audio);
        signal = SIGNAL_AUDIO_CHANGED;
        stream->audioTrack = RefPtr<WebCore::AudioTrackPrivateGStreamer>(static_cast<WebCore::AudioTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (g_str_has_prefix(mediaType, "video")) {
        g_assert(stream->type == Video);
        signal = SIGNAL_VIDEO_CHANGED;
        stream->videoTrack = RefPtr<WebCore::VideoTrackPrivateGStreamer>(static_cast<WebCore::VideoTrackPrivateGStreamer*>(trackPrivate.get()));
    } else if (g_str_has_prefix(mediaType, "text")) {
        g_assert(stream->type == Text);
        signal = SIGNAL_TEXT_CHANGED;

        // TODO: Support text tracks and mediaTypes related to EME
    }
    GST_OBJECT_UNLOCK(webKitMediaSrc);

    gst_caps_unref(appsrccaps);
    gst_caps_unref(oldAppsrccaps);

    if (signal != -1)
        g_signal_emit(G_OBJECT(source->parent), webkit_media_src_signals[signal], 0, NULL);
}

void PlaybackPipeline::markEndOfStream(MediaSourcePrivate::EndOfStreamStatus)
{
    WebKitMediaSrcPrivate* priv = m_webKitMediaSrc->priv;
    GList *l;

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Have EOS");

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    bool allTracksConfigured = priv->allTracksConfigured;
    if (!allTracksConfigured) {
        priv->allTracksConfigured = true;
    }
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    if (!allTracksConfigured) {
        gst_element_no_more_pads(GST_ELEMENT(m_webKitMediaSrc.get()));
        webKitMediaSrcDoAsyncDone(m_webKitMediaSrc.get());
    }

    Vector<GstAppSrc*> appSrcs;

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    for (l = priv->sources; l; l = l->next) {
        Source *source = static_cast<Source*>(l->data);
        if (source->src)
            appSrcs.append(GST_APP_SRC(source->src));
    }
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    for (Vector<GstAppSrc*>::iterator it = appSrcs.begin(); it != appSrcs.end(); ++it)
        gst_app_src_end_of_stream(*it);
}

void PlaybackPipeline::flushAndEnqueueNonDisplayingSamples(Vector<RefPtr<MediaSample> > samples)
{
    g_assert(WTF::isMainThread());

    if (samples.size() == 0) {
        LOG_MEDIA_MESSAGE("No samples, trackId unknown");
        return;
    }

    AtomicString trackId = samples[0]->trackID();
    LOG_MEDIA_MESSAGE("trackId=%s PTS[0]=%f ... PTS[n]=%f", trackId.string().utf8().data(), samples[0]->presentationTime().toFloat(), samples[samples.size()-1]->presentationTime().toFloat());

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Flushing and re-enqueing %d samples for stream %s", samples.size(), trackId.string().utf8().data());

    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    Stream* stream = getStreamByTrackId(m_webKitMediaSrc.get(), trackId);

    if (!stream) {
        GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());
        return;
    }

    Source* source = stream->parent;
    GstElement* appsrc = source->src;
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    // Actually no need to flush. The seek preparations have done it for us.

    for (Vector<RefPtr<MediaSample> >::iterator it = samples.begin(); it != samples.end(); ++it) {
        GStreamerMediaSample* sample = static_cast<GStreamerMediaSample*>(it->get());
        if (sample->sample() && gst_sample_get_buffer(sample->sample())) {
            GstSample* gstsample = gst_sample_ref(sample->sample());
            GST_BUFFER_FLAG_SET(gst_sample_get_buffer(gstsample), GST_BUFFER_FLAG_DECODE_ONLY);
            gst_app_src_push_sample(GST_APP_SRC(appsrc), gstsample);
        }
    }
}

void PlaybackPipeline::enqueueSample(PassRefPtr<MediaSample> prsample)
{
    RefPtr<MediaSample> rsample = prsample;
    AtomicString trackId = rsample->trackID();

    LOG_MEDIA_MESSAGE("trackId=%s PTS=%f presentationSize=%.0fx%.0f", trackId.string().utf8().data(), rsample->presentationTime().toFloat(), rsample->presentationSize().width(), rsample->presentationSize().height());

    g_assert(WTF::isMainThread());

    GST_DEBUG_OBJECT(m_webKitMediaSrc.get(), "Enqueing sample to stream %s at %" GST_TIME_FORMAT, trackId.string().utf8().data(), GST_TIME_ARGS(floatToGstClockTime(rsample->presentationTime().toDouble())));
    GST_OBJECT_LOCK(m_webKitMediaSrc.get());
    Stream* stream = getStreamByTrackId(m_webKitMediaSrc.get(), trackId);

    if (!stream) {
        LOG_MEDIA_MESSAGE("No stream!");
        GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());
        return;
    }

    GstElement* appsrc = stream->parent->src;
    GST_OBJECT_UNLOCK(m_webKitMediaSrc.get());

    GStreamerMediaSample* sample = static_cast<GStreamerMediaSample*>(rsample.get());
    if (sample->sample() && gst_sample_get_buffer(sample->sample())) {
        GstSample* gstsample = gst_sample_ref(sample->sample());
        GST_BUFFER_FLAG_UNSET(gst_sample_get_buffer(gstsample), GST_BUFFER_FLAG_DECODE_ONLY);
        gst_app_src_push_sample(GST_APP_SRC(appsrc), gstsample);
    }
}

GstElement* PlaybackPipeline::pipeline()
{
    if (!m_webKitMediaSrc)
        return nullptr;

    return GST_ELEMENT_PARENT(GST_ELEMENT_PARENT(GST_ELEMENT(m_webKitMediaSrc.get())));
}

};

void webkit_media_src_set_mediaplayerprivate(WebKitMediaSrc* src, WebCore::MediaPlayerPrivateGStreamerMSE* mediaPlayerPrivate)
{
    GST_OBJECT_LOCK(src);
    // Set to 0 on MediaPlayerPrivateGStreamer destruction, never a dangling pointer
    src->priv->mediaPlayerPrivate = mediaPlayerPrivate;
    GST_OBJECT_UNLOCK(src);
}

void webkit_media_src_prepare_seek(WebKitMediaSrc* src, const MediaTime& time)
{
    GST_OBJECT_LOCK(src);
    src->priv->seekTime = time;
    src->priv->appSrcSeekDataCount = 0;
    src->priv->appSrcNeedDataCount = 0;

    // The pending action will be performed in app_src_seek_data().
    src->priv->appSrcSeekDataNextAction = MediaSourceSeekToTime;
    GST_OBJECT_UNLOCK(src);
}

static GstClockTime floatToGstClockTime(float time)
{
    // Extract the integer part of the time (seconds) and the fractional part (microseconds). Attempt to
    // round the microseconds so no floating point precision is lost and we can perform an accurate seek.
    float seconds;
    float microSeconds = std::modf(time, &seconds) * 1000000;
    GTimeVal timeValue;
    timeValue.tv_sec = static_cast<glong>(seconds);
    timeValue.tv_usec = static_cast<glong>(roundf(microSeconds / 10000) * 10000);
    return GST_TIMEVAL_TO_TIME(timeValue);
}

#if 0
void webkit_media_src_segment_needed(WebKitMediaSrc* src, WebCore::StreamType streamType)
{
    // The video sink has received reset-time and needs a new segment before
    // new frames can be pushed. The new segment will be pushed to the
    // multiqueue video srcpad
    GST_OBJECT_LOCK(src);
    MediaTime seekTime = src->priv->seekTime;
    int flushAndReenqueueCount = src->priv->flushAndReenqueueCount;

    if (seekTime.isValid() && flushAndReenqueueCount > 0) {
        src->priv->flushAndReenqueueCount--;

        if (src->priv->flushAndReenqueueCount == 0) {
            if (src->priv->seekTime.isValid())
                src->priv->seekTime = MediaTime::invalidTime();

            GstEvent* seekEvent = src->priv->seekEvent;
            if (seekEvent) {
                src->priv->seekEvent = NULL;
                gst_event_unref(seekEvent);
            }
        }
    }
    GST_OBJECT_UNLOCK(src);

    if (seekTime.isValid()) {
        // The flushAndReenqueue method will take care of pushing the segment
        if (flushAndReenqueueCount > 0)
            return;

        GstPad* demuxersrcpad = NULL;

        switch (streamType) {
        case STREAM_TYPE_AUDIO:
            demuxersrcpad = webkit_media_src_get_audio_pad(src, 0);
            break;
        case STREAM_TYPE_VIDEO:
            demuxersrcpad = webkit_media_src_get_video_pad(src, 0);
            break;
        default:
            break;
        }

        if (!demuxersrcpad)
            return;

        GstSegment* segment = gst_segment_new();

        gst_segment_init(segment, GST_FORMAT_TIME);
        segment->start = floatToGstClockTime(seekTime.toFloat());
        segment->stop = GST_CLOCK_TIME_NONE;

        gst_pad_push_event(demuxersrcpad, gst_event_new_segment(segment));
        gst_segment_free(segment);
    }
}
#endif

#if 0
static gboolean webKitMediaSrcNotifyAppendCompleteToPlayer(WebKitMediaSrc* src)
{
    WebCore::MediaPlayerPrivateGStreamer* mediaPlayerPrivate = 0;
    bool isAppending;

    GST_OBJECT_LOCK(src);
    mediaPlayerPrivate = src->priv->mediaPlayerPrivate;
    isAppending = (src->priv->ongoingAppends > 0);
    GST_OBJECT_UNLOCK(src);

    if (mediaPlayerPrivate && !isAppending)
        mediaPlayerPrivate->notifyAppendComplete();

    gst_object_unref(src);
    return G_SOURCE_REMOVE;
}
#endif

namespace WTF {
template <> GRefPtr<WebKitMediaSrc> adoptGRef(WebKitMediaSrc* ptr)
{
    ASSERT(!ptr || !g_object_is_floating(G_OBJECT(ptr)));
    return GRefPtr<WebKitMediaSrc>(ptr, GRefPtrAdopt);
}

template <> WebKitMediaSrc* refGPtr<WebKitMediaSrc>(WebKitMediaSrc* ptr)
{
    if (ptr)
        gst_object_ref_sink(GST_OBJECT(ptr));

    return ptr;
}

template <> void derefGPtr<WebKitMediaSrc>(WebKitMediaSrc* ptr)
{
    if (ptr)
        gst_object_unref(ptr);
}
};

#endif // USE(GSTREAMER)

