/*
 * Copyright (C) 2007, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2007 Collabora Ltd.  All rights reserved.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2009 Gustavo Noronha Silva <gns@gnome.org>
 * Copyright (C) 2009, 2010, 2011, 2012, 2013 Igalia S.L
 * Copyright (C) 2014 Cable Television Laboratories, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "MediaPlayerPrivateGStreamerMSE.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(MEDIA_SOURCE) && ENABLE(VIDEO_TRACK)

#include "GStreamerUtilities.h"
#include "URL.h"
#include "MIMETypeRegistry.h"
#include "MediaDescription.h"
#include "MediaPlayer.h"
#include "MediaPlayerRequestInstallMissingPluginsCallback.h"
#include "NotImplemented.h"
#include "SecurityOrigin.h"
#include "TimeRanges.h"
#include "UUID.h"
#include "WebKitWebSourceGStreamer.h"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/pbutils/missing-plugins.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <limits>
#include <wtf/CurrentTime.h>
#include <wtf/HexNumber.h>
#include <wtf/MediaTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/CString.h>


#include "AudioTrackPrivateGStreamer.h"
#include "InbandMetadataTextTrackPrivateGStreamer.h"
#include "InbandTextTrackPrivateGStreamer.h"
#include "SourceBufferPrivateGStreamer.h"
#include "TextCombinerGStreamer.h"
#include "TextSinkGStreamer.h"
#include "VideoTrackPrivateGStreamer.h"

#if USE(GSTREAMER_MPEGTS)
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>
#undef GST_USE_UNSTABLE_API
#endif
#include <gst/audio/streamvolume.h>

#if ENABLE(ENCRYPTED_MEDIA)
#include "WebKitCommonEncryptionDecryptorGStreamer.h"
#endif

#include "MediaSource.h"

#if ENABLE(WEB_AUDIO)
#include "AudioSourceProviderGStreamer.h"
#endif

#if USE(DXDRM) && (ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2))
#if ENABLE(ENCRYPTED_MEDIA_V2)
#include "CDMPRSessionGStreamer.h"
#elif ENABLE(ENCRYPTED_MEDIA)
#include "DiscretixSession.h"
#endif
#include "WebKitPlayReadyDecryptorGStreamer.h"
#endif

#if USE(GSTREAMER_GL)
#include "GLContext.h"

#define GST_USE_UNSTABLE_API
#include <gst/gl/gl.h>
#undef GST_USE_UNSTABLE_API

#if USE(GLX)
#include "GLContextGLX.h"
#include <gst/gl/x11/gstgldisplay_x11.h>
#elif USE(EGL)
#include "GLContextEGL.h"
#include <gst/gl/egl/gstgldisplay_egl.h>
#endif

// gstglapi.h may include eglplatform.h and it includes X.h, which
// defines None, breaking MediaPlayer::None enum
#if PLATFORM(X11) && GST_GL_HAVE_PLATFORM_EGL
#undef None
#endif
#endif // USE(GSTREAMER_GL)

#if PLATFORM(WAYLAND)
#include "PlatformDisplayWayland.h"
#endif

#include <condition_variable>
#include <mutex>
#include <wtf/MainThread.h>

static const char* dumpReadyState(WebCore::MediaPlayer::ReadyState readyState)
{
    switch (readyState) {
    case WebCore::MediaPlayer::HaveNothing: return "HaveNothing";
    case WebCore::MediaPlayer::HaveMetadata: return "HaveMetadata";
    case WebCore::MediaPlayer::HaveCurrentData: return "HaveCurrentData";
    case WebCore::MediaPlayer::HaveFutureData: return "HaveFutureData";
    case WebCore::MediaPlayer::HaveEnoughData: return "HaveEnoughData";
    default: return "(unknown)";
    }
}

// Max interval in seconds to stay in the READY state on manual
// state change requests.
static const unsigned gReadyStateTimerInterval = 60;

GST_DEBUG_CATEGORY_EXTERN(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

using namespace std;

namespace WebCore {

class AppendPipeline : public ThreadSafeRefCounted<AppendPipeline> {
public:
    enum AppendStage { Invalid, NotStarted, Ongoing, NoDataToDecode, Sampling, LastSample, Aborting };

    static const unsigned int s_noDataToDecodeTimeoutMsec = 500;
    static const unsigned int s_lastSampleTimeoutMsec = 100;

    AppendPipeline(PassRefPtr<MediaSourceClientGStreamerMSE> mediaSourceClient, PassRefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, MediaPlayerPrivateGStreamerMSE* playerPrivate);
    virtual ~AppendPipeline();

    gint id();
    void setAppendStage(AppendStage newAppendStage);

    // Takes ownership of caps.
    void updatePresentationSize(GstCaps* demuxersrcpadcaps);
    void demuxerPadAdded(GstPad* demuxersrcpad);
    void demuxerPadRemoved(GstPad*);
    void appSinkCapsChanged();
    void appSinkNewSample(GstSample* sample);
    void appSinkEOS();
    void didReceiveInitializationSegment();
    AtomicString trackId();
    void abort();

    RefPtr<MediaSourceClientGStreamerMSE> mediaSourceClient() { return m_mediaSourceClient; }
    RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate() { return m_sourceBufferPrivate; }
    GstElement* pipeline() { return m_pipeline; }
    GstElement* appsrc() { return m_appsrc; }
    GstElement* qtdemux() { return m_qtdemux; }
    GstElement* appsink() { return m_appsink; }
    GstCaps* demuxersrcpadcaps() { return m_demuxersrcpadcaps; }
    RefPtr<WebCore::TrackPrivateBase> track() { return m_track; }

private:
    void resetPipeline();

// TODO: Hide everything and use getters/setters.
private:
    RefPtr<MediaSourceClientGStreamerMSE> m_mediaSourceClient;
    RefPtr<SourceBufferPrivateGStreamer> m_sourceBufferPrivate;
    MediaPlayerPrivateGStreamerMSE* m_playerPrivate;

    // (m_mediaType, m_id) is unique.
    gint m_id;

    GstElement* m_pipeline;
    GstElement* m_appsrc;
    GstElement* m_typefind;
    GstElement* m_qtdemux;

    // The demuxer has one src Stream only.
    GstElement* m_appsink;

    GstCaps* m_demuxersrcpadcaps;
    FloatSize m_presentationSize;

    // Some appended data are only headers and don't generate any
    // useful stream data for decoding. This is detected with a
    // timeout and reported to the upper layers, so update/updateend
    // can be generated and the append operation doesn't block.
    guint m_noDataToDecodeTimeoutTag;

    // Used to detect the last sample. Rescheduled each time a new
    // sample arrives.
    guint m_lastSampleTimeoutTag;

    // Keeps track of the stages of append processing, to avoid
    // performing actions inappropriate for the current stage (eg:
    // processing more samples when the last one has been detected
    // or the noDataToDecodeTimeout has been triggered).
    // See setAppendStage() for valid transitions.
    AppendStage m_appendStage;

    // Aborts can only be completed when the normal sample detection
    // has finished. Meanwhile, the willing to abort is expressed in
    // this field.
    bool m_abortPending;

    StreamType m_streamType;
    RefPtr<WebCore::TrackPrivateBase> m_track;
};

static gboolean mediaPlayerPrivateMessageCallback(GstBus*, GstMessage* message, MediaPlayerPrivateGStreamerMSE* player)
{
    return player->handleMessage(message);
}

static void mediaPlayerPrivateSyncMessageCallback(GstBus*, GstMessage* message, MediaPlayerPrivateGStreamerMSE* player)
{
    player->handleSyncMessage(message);
}

static void mediaPlayerPrivateSourceChangedCallback(GObject*, GParamSpec*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->sourceChanged();
}

static void mediaPlayerPrivateVideoSinkCapsChangedCallback(GObject*, GParamSpec*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->videoCapsChanged();
}

static void mediaPlayerPrivateVideoChangedCallback(GObject*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->videoChanged();
}

static void mediaPlayerPrivateAudioChangedCallback(GObject*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->audioChanged();
}

static void setAudioStreamPropertiesCallback(GstChildProxy*, GObject* object, gchar*,
    MediaPlayerPrivateGStreamerMSE* player)
{
    player->setAudioStreamProperties(object);
}

static void mediaPlayerPrivateTextChangedCallback(GObject*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->textChanged();
}

static GstFlowReturn mediaPlayerPrivateNewTextSampleCallback(GObject*, MediaPlayerPrivateGStreamerMSE* player)
{
    player->newTextSample();
    return GST_FLOW_OK;
}

/* // This is now directly handled by MediaSourceClientGStreamerMSE
static gboolean mediaPlayerPrivateNotifyDurationChanged(MediaPlayerPrivateGStreamerMSE* player)
{
    player->notifyDurationChanged();
    return G_SOURCE_REMOVE;
}
*/

void MediaPlayerPrivateGStreamerMSE::setAudioStreamProperties(GObject* object)
{
    if (g_strcmp0(G_OBJECT_TYPE_NAME(object), "GstPulseSink"))
        return;

    const char* role = m_player->client().mediaPlayerIsVideo() ? "video" : "music";
    GstStructure* structure = gst_structure_new("stream-properties", "media.role", G_TYPE_STRING, role, NULL);
    g_object_set(object, "stream-properties", structure, NULL);
    gst_structure_free(structure);
    GUniquePtr<gchar> elementName(gst_element_get_name(GST_ELEMENT(object)));
    LOG_MEDIA_MESSAGE("Set media.role as %s at %s", role, elementName.get());
}

void MediaPlayerPrivateGStreamerMSE::registerMediaEngine(MediaEngineRegistrar registrar)
{
    if (isAvailable())
         registrar([](MediaPlayer* player) { return std::make_unique<MediaPlayerPrivateGStreamerMSE>(player); },
            getSupportedTypes, supportsType, 0, 0, 0, supportsKeySystem);
}

static bool initializeGStreamerAndRegisterWebKitElements()
{
    if (!initializeGStreamer())
        return false;

    /*
    GRefPtr<GstElementFactory> srcFactory = gst_element_factory_find("webkitwebsrc");
    if (!srcFactory) {
        GST_DEBUG_CATEGORY_INIT(webkit_media_player_debug, "webkitmediaplayer", 0, "WebKit media player");
        gst_element_register(0, "webkitwebsrc", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_WEB_SRC);
    }
    */

#if ENABLE(ENCRYPTED_MEDIA)
    GRefPtr<GstElementFactory> cencDecryptorFactory = gst_element_factory_find("webkitcencdec");
    if (!cencDecryptorFactory)
        gst_element_register(0, "webkitcencdec", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_MEDIA_CENC_DECRYPT);
#endif

#if USE(DXDRM)
    GRefPtr<GstElementFactory> playReadyDecryptorFactory = gst_element_factory_find("webkitplayreadydec");
    if (!playReadyDecryptorFactory)
        gst_element_register(0, "webkitplayreadydec", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_MEDIA_PLAYREADY_DECRYPT);
#endif

    GRefPtr<GstElementFactory> WebKitMediaSrcFactory = gst_element_factory_find("webkitmediasrc");
    if (!WebKitMediaSrcFactory)
        gst_element_register(0, "webkitmediasrc", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_MEDIA_SRC);
    return true;
}

bool MediaPlayerPrivateGStreamerMSE::isAvailable()
{
    if (!initializeGStreamerAndRegisterWebKitElements())
        return false;

    GRefPtr<GstElementFactory> factory = gst_element_factory_find("playbin");
    return factory;
}

MediaPlayerPrivateGStreamerMSE::MediaPlayerPrivateGStreamerMSE(MediaPlayer* player)
    : MediaPlayerPrivateGStreamerBase(player)
    , m_source(0)
    , m_seekTime(0)
    , m_changingRate(false)
    , m_endTime(numeric_limits<float>::infinity())
    , m_isStreaming(false)
    , m_mediaLocations(0)
    , m_mediaLocationCurrentIndex(0)
    , m_resetPipeline(false)
    , m_paused(true)
    , m_playbackRatePause(false)
    , m_seeking(false)
    , m_seekIsPending(false)
    , m_timeOfOverlappingSeek(-1)
    , m_canFallBackToLastFinishedSeekPosition(false)
    , m_buffering(false)
    , m_playbackRate(1)
    , m_lastPlaybackRate(1)
    , m_errorOccured(false)
    , m_mediaDuration(0)
    , m_downloadFinished(false)
    , m_fillTimer(*this, &MediaPlayerPrivateGStreamerMSE::fillTimerFired)
    , m_maxTimeLoaded(0)
    , m_bufferingPercentage(0)
    , m_preload(player->preload())
    , m_delayingLoad(false)
    , m_mediaDurationKnown(true)
    , m_maxTimeLoadedAtLastDidLoadingProgress(0)
    , m_volumeAndMuteInitialized(false)
    , m_hasVideo(false)
    , m_hasAudio(false)
    , m_audioTimerHandler("[WebKit] MediaPlayerPrivateGStreamerMSE::audioChanged", std::bind(&MediaPlayerPrivateGStreamerMSE::notifyPlayerOfAudio, this))
    , m_textTimerHandler("[WebKit] MediaPlayerPrivateGStreamerMSE::textChanged", std::bind(&MediaPlayerPrivateGStreamerMSE::notifyPlayerOfText, this))
    , m_videoTimerHandler("[WebKit] MediaPlayerPrivateGStreamerMSE::videoChanged", std::bind(&MediaPlayerPrivateGStreamerMSE::notifyPlayerOfVideo, this))
    , m_videoCapsTimerHandler("[WebKit] MediaPlayerPrivateGStreamerMSE::videoCapsChanged", std::bind(&MediaPlayerPrivateGStreamerMSE::notifyPlayerOfVideoCaps, this))
    , m_readyTimerHandler("[WebKit] mediaPlayerPrivateReadyStateTimeoutCallback", [this] { changePipelineState(GST_STATE_NULL); })
    , m_totalBytes(0)
    , m_preservesPitch(false)
    , m_cachedPosition(-1)
    , m_lastQuery(-1)
#if ENABLE(WEB_AUDIO)
    , m_audioSourceProvider(std::make_unique<AudioSourceProviderGStreamer>())
#endif
    , m_requestedState(GST_STATE_VOID_PENDING)
    , m_pendingAsyncOperations(nullptr)
    , m_seekCompleted(true)
{
#if ENABLE(ENCRYPTED_MEDIA) && USE(DXDRM)
    m_dxdrmSession = 0;
#endif
    printf("### %s: %p\n", __PRETTY_FUNCTION__, this); fflush(stdout);
}

MediaPlayerPrivateGStreamerMSE::~MediaPlayerPrivateGStreamerMSE()
{
    printf("### %s: %p\n", __PRETTY_FUNCTION__, this); fflush(stdout);

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    // Potentially unblock GStreamer thread for DRM license acquisition.
    m_drmKeySemaphore.signal();
#endif

    for (size_t i = 0; i < m_audioTracks.size(); ++i)
        m_audioTracks[i]->disconnect();

    for (size_t i = 0; i < m_textTracks.size(); ++i)
        m_textTracks[i]->disconnect();

    for (size_t i = 0; i < m_videoTracks.size(); ++i)
        m_videoTracks[i]->disconnect();
    if (m_fillTimer.isActive())
        m_fillTimer.stop();

    if (m_mediaLocations) {
        gst_structure_free(m_mediaLocations);
        m_mediaLocations = 0;
    }

    if (m_autoAudioSink)
        g_signal_handlers_disconnect_by_func(G_OBJECT(m_autoAudioSink.get()),
            reinterpret_cast<gpointer>(setAudioStreamPropertiesCallback), this);

    m_readyTimerHandler.cancel();
    if (m_missingPluginsCallback) {
        m_missingPluginsCallback->invalidate();
        m_missingPluginsCallback = nullptr;
    }

    // Ensure that neither this class nor the base class hold references to any sample
    // as in the change to NULL the decoder needs to be able to free the buffers
    clearSamples();

    if (m_videoSink) {
        GRefPtr<GstPad> videoSinkPad = adoptGRef(gst_element_get_static_pad(m_videoSink.get(), "sink"));
        g_signal_handlers_disconnect_by_func(videoSinkPad.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateVideoSinkCapsChangedCallback), this);
    }

    if (m_source && WEBKIT_IS_MEDIA_SRC(m_source.get())) {
        webkit_media_src_set_mediaplayerprivate(WEBKIT_MEDIA_SRC(m_source.get()), 0);
        g_signal_handlers_disconnect_by_func(m_source.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateVideoChangedCallback), this);
        g_signal_handlers_disconnect_by_func(m_source.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateAudioChangedCallback), this);
        g_signal_handlers_disconnect_by_func(m_source.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateTextChangedCallback), this);
    }

    if (m_pipeline) {
        GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
        ASSERT(bus);
        g_signal_handlers_disconnect_by_func(bus.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateMessageCallback), this);
        g_signal_handlers_disconnect_by_func(bus.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateSyncMessageCallback), this);
        gst_bus_remove_signal_watch(bus.get());

        g_signal_handlers_disconnect_by_func(m_pipeline.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateSourceChangedCallback), this);
        g_signal_handlers_disconnect_by_func(m_pipeline.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateVideoChangedCallback), this);
        g_signal_handlers_disconnect_by_func(m_pipeline.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateAudioChangedCallback), this);
        g_signal_handlers_disconnect_by_func(m_pipeline.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateNewTextSampleCallback), this);
        g_signal_handlers_disconnect_by_func(m_pipeline.get(), reinterpret_cast<gpointer>(mediaPlayerPrivateTextChangedCallback), this);

        gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);
    }

    // Cancel pending mediaPlayerPrivateNotifyDurationChanged() delayed calls
    m_pendingAsyncOperationsLock.lock();
    while (m_pendingAsyncOperations) {
        g_source_remove(GPOINTER_TO_UINT(m_pendingAsyncOperations->data));
        m_pendingAsyncOperations = g_list_remove(m_pendingAsyncOperations, m_pendingAsyncOperations->data);
    }
    m_pendingAsyncOperationsLock.unlock();
}

void MediaPlayerPrivateGStreamerMSE::load(const String& urlString)
{
    if (!initializeGStreamerAndRegisterWebKitElements())
        return;

    if (!urlString.startsWith("mediasource")) {
        LOG_MEDIA_MESSAGE("Unsupported url: %s", urlString.utf8().data());
        return;
    }

    URL url(URL(), urlString);
    if (url.isBlankURL())
        return;

    // Clean out everything after file:// url path.
    String cleanURL(urlString);
    if (url.isLocalFile())
        cleanURL = cleanURL.substring(0, url.pathEnd());

    if (!m_pipeline) {
        createGSTPlayBin();
        m_playbackPipeline = PlaybackPipeline::create();
    }

    ASSERT(m_pipeline);

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    // Potentially unblock GStreamer thread for DRM license acquisition.
    m_drmKeySemaphore.signal();
#endif

    m_url = URL(URL(), cleanURL);
    g_object_set(m_pipeline.get(), "uri", cleanURL.utf8().data(), nullptr);

    INFO_MEDIA_MESSAGE("Load %s", cleanURL.utf8().data());

    if (m_preload == MediaPlayer::None) {
        LOG_MEDIA_MESSAGE("Delaying load.");
        m_delayingLoad = true;
    }

    // Reset network and ready states. Those will be set properly once
    // the pipeline pre-rolled.
    m_networkState = MediaPlayer::Loading;
    m_player->networkStateChanged();
    m_readyState = MediaPlayer::HaveNothing;
    printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);

    m_player->readyStateChanged();
    m_volumeAndMuteInitialized = false;

    if (!m_delayingLoad)
        commitLoad();
}

void MediaPlayerPrivateGStreamerMSE::load(const String& url, MediaSourcePrivateClient* mediaSource)
{
    String mediasourceUri = String::format("mediasource%s", url.utf8().data());
    m_mediaSource = mediaSource;
    load(mediasourceUri);
}

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateGStreamerMSE::load(MediaStreamPrivate&)
{
    notImplemented();
}
#endif

void MediaPlayerPrivateGStreamerMSE::commitLoad()
{
    ASSERT(!m_delayingLoad);
    LOG_MEDIA_MESSAGE("Committing load.");

    // GStreamer needs to have the pipeline set to a paused state to
    // start providing anything useful.
    changePipelineState(GST_STATE_PAUSED);

    setDownloadBuffering();
    updateStates();
}

float MediaPlayerPrivateGStreamerMSE::playbackPosition() const
{
    if (m_isEndReached) {
        // Position queries on a null pipeline return 0. If we're at
        // the end of the stream the pipeline is null but we want to
        // report either the seek time or the duration because this is
        // what the Media element spec expects us to do.
        if (m_seeking)
            return m_seekTime;
        if (m_mediaDuration)
            return m_mediaDuration;
        return 0;
    }

    double now = WTF::currentTime();
    if (m_lastQuery > -1 && ((now - m_lastQuery) < 0.25) && (m_cachedPosition > -1))
        return m_cachedPosition;

    m_lastQuery = now;

    // Position is only available if no async state change is going on and the state is either paused or playing.
    gint64 position = GST_CLOCK_TIME_NONE;
    GstQuery* query= gst_query_new_position(GST_FORMAT_TIME);
    if (gst_element_query(m_pipeline.get(), query))
        gst_query_parse_position(query, 0, &position);

    float result = 0.0f;
    if (static_cast<GstClockTime>(position) != GST_CLOCK_TIME_NONE)
        result = static_cast<double>(position) / GST_SECOND;
    else if (m_canFallBackToLastFinishedSeekPosition)
        result = m_seekTime;

    LOG_MEDIA_MESSAGE("Position %" GST_TIME_FORMAT, GST_TIME_ARGS(position));

    gst_query_unref(query);
    m_cachedPosition = result;
    return result;
}

bool MediaPlayerPrivateGStreamerMSE::changePipelineState(GstState newState)
{
    ASSERT(m_pipeline);

    GstState currentState;
    GstState pending;

    gst_element_get_state(m_pipeline.get(), &currentState, &pending, 0);
    if (currentState == newState || pending == newState) {
        LOG_MEDIA_MESSAGE("Rejected state change to %s from %s with %s pending", gst_element_state_get_name(newState),
            gst_element_state_get_name(currentState), gst_element_state_get_name(pending));
        return true;
    }

    LOG_MEDIA_MESSAGE("Changing state change to %s from %s with %s pending", gst_element_state_get_name(newState),
        gst_element_state_get_name(currentState), gst_element_state_get_name(pending));

    GstStateChangeReturn setStateResult = gst_element_set_state(m_pipeline.get(), newState);
    GstState pausedOrPlaying = newState == GST_STATE_PLAYING ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (currentState != pausedOrPlaying && setStateResult == GST_STATE_CHANGE_FAILURE) {
        return false;
    }

    // Create a timer when entering the READY state so that we can free resources
    // if we stay for too long on READY.
    // Also lets remove the timer if we request a state change for any state other than READY.
    // See also https://bugs.webkit.org/show_bug.cgi?id=117354
    if (newState == GST_STATE_READY && !m_readyTimerHandler.isScheduled()) {
        m_readyTimerHandler.schedule(std::chrono::seconds(gReadyStateTimerInterval));
    } else if (newState != GST_STATE_READY && m_readyTimerHandler.isScheduled()) {
        m_readyTimerHandler.cancel();
    }

    return true;
}

void MediaPlayerPrivateGStreamerMSE::prepareToPlay()
{
    m_preload = MediaPlayer::Auto;
    if (m_delayingLoad) {
        m_delayingLoad = false;
        commitLoad();
    }
}

void MediaPlayerPrivateGStreamerMSE::play()
{
    if (!m_playbackRate) {
        m_playbackRatePause = true;
        return;
    }

    if (changePipelineState(GST_STATE_PLAYING)) {
        m_isEndReached = false;
        m_delayingLoad = false;
        m_preload = MediaPlayer::Auto;
        setDownloadBuffering();
        LOG_MEDIA_MESSAGE("Play");
    } else {
        loadingFailed(MediaPlayer::Empty);
    }
}

void MediaPlayerPrivateGStreamerMSE::pause()
{
    m_playbackRatePause = false;
    GstState currentState, pendingState;
    gst_element_get_state(m_pipeline.get(), &currentState, &pendingState, 0);
    if (currentState < GST_STATE_PAUSED && pendingState <= GST_STATE_PAUSED)
        return;

    if (changePipelineState(GST_STATE_PAUSED))
        INFO_MEDIA_MESSAGE("Pause");
    else
        loadingFailed(MediaPlayer::Empty);
}

float MediaPlayerPrivateGStreamerMSE::duration() const
{
    if (!m_pipeline)
        return 0.0f;

    if (m_errorOccured)
        return 0.0f;

    return m_mediaDuration;

    /*
    // Media duration query failed already, don't attempt new useless queries.
    if (!m_mediaDurationKnown)
        return numeric_limits<float>::infinity();

    if (m_mediaDuration)
        return m_mediaDuration;

    GstFormat timeFormat = GST_FORMAT_TIME;
    gint64 timeLength = 0;

    bool failure = !gst_element_query_duration(m_pipeline.get(), timeFormat, &timeLength) || static_cast<guint64>(timeLength) == GST_CLOCK_TIME_NONE;

    if (failure && m_source)
        failure = !gst_element_query_duration(m_source.get(), timeFormat, &timeLength) || static_cast<guint64>(timeLength) == GST_CLOCK_TIME_NONE;

    if (failure && m_mediaSource)
        return m_mediaSource->duration().toFloat();

    if (failure) {
        LOG_MEDIA_MESSAGE("Time duration query failed for %s", m_url.string().utf8().data());
        return numeric_limits<float>::infinity();
    }

    LOG_MEDIA_MESSAGE("Duration: %" GST_TIME_FORMAT, GST_TIME_ARGS(timeLength));

    m_mediaDuration = static_cast<double>(timeLength) / GST_SECOND;
    return m_mediaDuration;
    // FIXME: handle 3.14.9.5 properly
    */
}

float MediaPlayerPrivateGStreamerMSE::currentTime() const
{
    if (!m_pipeline) {
        printf("### %s: (NO PIPELINE) %f\n", __PRETTY_FUNCTION__, 0.0f); fflush(stdout);
        return 0.0f;
    }

    if (m_errorOccured) {
        printf("### %s: (ERROR) %f\n", __PRETTY_FUNCTION__, 0.0f); fflush(stdout);
        return 0.0f;
    }

    // if (m_seeking) {
    //     printf("### %s: (SEEKING) %f\n", __PRETTY_FUNCTION__, m_seekTime); fflush(stdout);
    //     return m_seekTime;
    // }

    // Workaround for
    // https://bugzilla.gnome.org/show_bug.cgi?id=639941 In GStreamer
    // 0.10.35 basesink reports wrong duration in case of EOS and
    // negative playback rate. There's no upstream accepted patch for
    // this bug yet, hence this temporary workaround.
    if (m_isEndReached && m_playbackRate < 0) {
        printf("### %s: (END OR NEGATIVE) %f\n", __PRETTY_FUNCTION__, 0.0f); fflush(stdout);
        return 0.0f;
    }

    float playpos = playbackPosition();
    printf("### %s: (PLAYBACK POSITION) %f\n", __PRETTY_FUNCTION__, playpos); fflush(stdout);
    return playpos;
}

void MediaPlayerPrivateGStreamerMSE::seek(float time)
{
    if (!m_pipeline)
        return;

    if (m_errorOccured)
        return;

    INFO_MEDIA_MESSAGE("[Seek] seek attempt to %f secs", time);

    // Avoid useless seeking.
    if (time == currentTime())
        return;

    if (isLiveStream())
        return;

    GstClockTime clockTime = toGstClockTime(time);
    INFO_MEDIA_MESSAGE("[Seek] seeking to %" GST_TIME_FORMAT " (%f)", GST_TIME_ARGS(clockTime), time);

    if (m_seeking) {
        m_timeOfOverlappingSeek = time;
        if (m_seekIsPending) {
            m_seekTime = time;
            return;
        }
    }

    GstState state;
    GstState newState;
    GstStateChangeReturn getStateResult = gst_element_get_state(m_pipeline.get(), &state, &newState, 0);
    if (getStateResult == GST_STATE_CHANGE_FAILURE || getStateResult == GST_STATE_CHANGE_NO_PREROLL) {
        LOG_MEDIA_MESSAGE("[Seek] cannot seek, current state change is %s", gst_element_state_change_return_get_name(getStateResult));
        return;
    }

    if (getStateResult == GST_STATE_CHANGE_ASYNC || state < GST_STATE_PAUSED
            || m_isEndReached) {
        CString reason = "Unknown reason";
        if (getStateResult == GST_STATE_CHANGE_ASYNC) reason = String::format("In async change %s --> %s", gst_element_state_get_name(state), gst_element_state_get_name(newState)).utf8();
        else if (state < GST_STATE_PAUSED) reason = "State less than PAUSED";
        else if (m_isEndReached) reason = "End reached";

        printf("### %s: Delaying the seek: %s\n", __PRETTY_FUNCTION__, reason.data()); fflush(stdout);
        LOG_MEDIA_MESSAGE("[Seek] delaying because: %s\n", reason.data());
        m_seekIsPending = true;
        if (m_isEndReached) {
            LOG_MEDIA_MESSAGE("[Seek] reset pipeline");
            m_resetPipeline = true;
            if (!changePipelineState(GST_STATE_PAUSED))
                loadingFailed(MediaPlayer::Empty);
        }
    } else {
        printf("### %s: We can seek now\n", __PRETTY_FUNCTION__); fflush(stdout);
        LOG_MEDIA_MESSAGE("[Seek] now possible");
        if (!doSeek(clockTime, m_player->rate(), static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE))) {
            LOG_MEDIA_MESSAGE("[Seek] seeking to %f failed", time);
            printf("### %s: Seeking to %f failed\n", __PRETTY_FUNCTION__, time); fflush(stdout);
            return;
        }
    }

    m_seeking = true;
    m_seekTime = time;
    m_isEndReached = false;
    printf("### %s m_seeking=%s, m_seekTime=%f\n", __PRETTY_FUNCTION__, m_seeking?"true":"false", m_seekTime); fflush(stdout);
}

static gboolean dumpPipeline(gpointer data)
{
    GstElement* pipeline = reinterpret_cast<GstElement*>(data);

    g_printerr("Dumping pipeline\n");
    CString dotFileName = "pipeline-dump";
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.data());

    return G_SOURCE_REMOVE;
}

void MediaPlayerPrivateGStreamerMSE::notifySeekNeedsData(const MediaTime& seekTime)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    // Reenqueue samples needed to resume playback in the new position
    m_mediaSource->seekToTime(seekTime);

    LOG_MEDIA_MESSAGE("MSE seek to %f finished", seekTime.toDouble());
    g_timeout_add_seconds(5, dumpPipeline, m_pipeline.get());
}

bool MediaPlayerPrivateGStreamerMSE::doSeek(gint64 position, float rate, GstSeekFlags seekType)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    gint64 startTime, endTime;

    if (rate > 0) {
        startTime = position;
        endTime = GST_CLOCK_TIME_NONE;
    } else {
        startTime = 0;
        // If we are at beginning of media, start from the end to
        // avoid immediate EOS.
        if (position < 0)
            endTime = static_cast<gint64>(duration() * GST_SECOND);
        else
            endTime = position;
    }

    if (!rate)
        rate = 1.0;

    LOG_MEDIA_MESSAGE("Actual seek to %" GST_TIME_FORMAT ", end time:  %" GST_TIME_FORMAT ", rate: %f", GST_TIME_ARGS(startTime), GST_TIME_ARGS(endTime), rate);

    MediaTime time(MediaTime::createWithDouble(double(static_cast<double>(position) / GST_SECOND)));

    // DEBUG
    {
        float currentTime = 0.0f;
        gint64 pos = GST_CLOCK_TIME_NONE;
        GstQuery* query= gst_query_new_position(GST_FORMAT_TIME);
        if (gst_element_query(m_pipeline.get(), query))
            gst_query_parse_position(query, 0, &pos);
        currentTime = static_cast<double>(pos) / GST_SECOND;
        printf("### %s: seekTime=%f, currentTime=%f\n", __PRETTY_FUNCTION__, time.toFloat(), currentTime); fflush(stdout);
    }

    // This will call notifySeekNeedsData() after some time to tell that the pipeline is ready for sample enqueuing.
    webkit_media_src_prepare_seek(WEBKIT_MEDIA_SRC(m_source.get()), time);

    // DEBUG
    dumpPipeline(m_pipeline.get());

    if (!gst_element_seek(m_pipeline.get(), rate, GST_FORMAT_TIME, seekType,
        GST_SEEK_TYPE_SET, startTime, GST_SEEK_TYPE_SET, endTime)) {
        printf("### %s: Returning false\n", __PRETTY_FUNCTION__); fflush(stdout);
        return false;
    }

    // The samples will be enqueued in notifySeekNeedsData()
    printf("### %s: Returning true\n", __PRETTY_FUNCTION__); fflush(stdout);
    return true;
}

void MediaPlayerPrivateGStreamerMSE::updatePlaybackRate()
{
    if (!m_changingRate)
        return;

    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    // Not implemented
    return;

    /*
    float currentPosition = static_cast<float>(playbackPosition() * GST_SECOND);
    bool mute = false;

    INFO_MEDIA_MESSAGE("Set Rate to %f", m_playbackRate);

    if (m_playbackRate > 0) {
        // Mute the sound if the playback rate is too extreme and
        // audio pitch is not adjusted.
        mute = (!m_preservesPitch && (m_playbackRate < 0.8 || m_playbackRate > 2));
    } else {
        if (currentPosition == 0.0f)
            currentPosition = -1.0f;
        mute = true;
    }

    INFO_MEDIA_MESSAGE("Need to mute audio?: %d", (int) mute);
    if (doSeek(currentPosition, m_playbackRate, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH))) {
        g_object_set(m_pipeline.get(), "mute", mute, nullptr);
        m_lastPlaybackRate = m_playbackRate;
    } else {
        m_playbackRate = m_lastPlaybackRate;
        ERROR_MEDIA_MESSAGE("Set rate to %f failed", m_playbackRate);
    }

    if (m_playbackRatePause) {
        GstState state;
        GstState pending;

        gst_element_get_state(m_pipeline.get(), &state, &pending, 0);
        if (state != GST_STATE_PLAYING && pending != GST_STATE_PLAYING)
            changePipelineState(GST_STATE_PLAYING);
        m_playbackRatePause = false;
    }

    m_changingRate = false;
    m_player->rateChanged();
    */
}

bool MediaPlayerPrivateGStreamerMSE::paused() const
{
    if (m_isEndReached) {
        LOG_MEDIA_MESSAGE("Ignoring pause at EOS");
        return true;
    }

    if (m_playbackRatePause)
        return false;

    GstState state;
    gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);
    return state <= GST_STATE_PAUSED;
}

bool MediaPlayerPrivateGStreamerMSE::seeking() const
{
    return m_seeking && !m_seekCompleted;
}

void MediaPlayerPrivateGStreamerMSE::videoChanged()
{
    if (isMainThread())
        notifyPlayerOfVideo();
    else
        m_videoTimerHandler.schedule();
}

void MediaPlayerPrivateGStreamerMSE::videoCapsChanged()
{
    m_videoCapsTimerHandler.schedule();
}

void MediaPlayerPrivateGStreamerMSE::notifyPlayerOfVideo()
{
    if (!m_pipeline || !m_source)
        return;

    gint numTracks = 0;
    GstElement* element = m_source.get();
    g_object_get(element, "n-video", &numTracks, nullptr);

    LOG_MEDIA_MESSAGE("Stream has %d video tracks", numTracks);
    m_hasVideo = numTracks > 0;

    LOG_MEDIA_MESSAGE("Tracks managed by source element. Bailing out now.");
    m_player->client().mediaPlayerEngineUpdated(m_player);
}

void MediaPlayerPrivateGStreamerMSE::notifyPlayerOfVideoCaps()
{
    m_videoSize = IntSize();
    m_player->client().mediaPlayerEngineUpdated(m_player);
}

void MediaPlayerPrivateGStreamerMSE::audioChanged()
{
    if (isMainThread())
        notifyPlayerOfAudio();
    else
        m_audioTimerHandler.schedule();
}

void MediaPlayerPrivateGStreamerMSE::notifyPlayerOfAudio()
{
    if (!m_pipeline || !m_source)
        return;

    gint numTracks = 0;
    GstElement* element = m_source.get();
    m_hasAudio = numTracks > 0;

    g_object_get(element, "n-audio", &numTracks, nullptr);

    LOG_MEDIA_MESSAGE("Stream has %d audio tracks", numTracks);
    m_hasAudio = numTracks > 0;

    LOG_MEDIA_MESSAGE("Tracks managed by source element. Bailing out now.");
    m_player->client().mediaPlayerEngineUpdated(m_player);
    return;
}

void MediaPlayerPrivateGStreamerMSE::textChanged()
{
    if (isMainThread())
        notifyPlayerOfText();
    else
        m_textTimerHandler.schedule();
}

void MediaPlayerPrivateGStreamerMSE::notifyPlayerOfText()
{
    if (!m_pipeline || !m_source)
        return;

    gint numTracks = 0;
    GstElement* element = m_source.get();
    g_object_get(element, "n-text", &numTracks, nullptr);

    LOG_MEDIA_MESSAGE("Stream has %d text tracks", numTracks);

    LOG_MEDIA_MESSAGE("Tracks managed by source element. Bailing out now.");
}

void MediaPlayerPrivateGStreamerMSE::newTextSample()
{
    if (!m_textAppSink)
        return;

    GRefPtr<GstEvent> streamStartEvent = adoptGRef(
        gst_pad_get_sticky_event(m_textAppSinkPad.get(), GST_EVENT_STREAM_START, 0));

    GRefPtr<GstSample> sample;
    g_signal_emit_by_name(m_textAppSink.get(), "pull-sample", &sample.outPtr(), NULL);
    ASSERT(sample);

    if (streamStartEvent) {
        bool found = FALSE;
        const gchar* id;
        gst_event_parse_stream_start(streamStartEvent.get(), &id);
        for (size_t i = 0; i < m_textTracks.size(); ++i) {
            RefPtr<InbandTextTrackPrivateGStreamer> track = m_textTracks[i];
            if (track->streamId() == id) {
                track->handleSample(sample);
                found = true;
                break;
            }
        }
        if (!found)
            WARN_MEDIA_MESSAGE("Got sample with unknown stream ID.");
    } else
        WARN_MEDIA_MESSAGE("Unable to handle sample with no stream start event.");
}

// METRO FIXME: GStreamer mediaplayer manages the readystate on its own. We shouldn't change it manually.
void MediaPlayerPrivateGStreamerMSE::setReadyState(MediaPlayer::ReadyState state)
{
    // FIXME: early return here.
    if (state != m_readyState) {
        LOG_MEDIA_MESSAGE("Ready State Changed manually from %u to %u", m_readyState, state);
        MediaPlayer::ReadyState oldReadyState = m_readyState;
        m_readyState = state;
        printf("### %s: m_readyState: %s -> %s\n", __PRETTY_FUNCTION__, dumpReadyState(oldReadyState), dumpReadyState(m_readyState)); fflush(stdout);

        GstState state;
        GstStateChangeReturn getStateResult = gst_element_get_state(m_pipeline.get(), &state, NULL, 250 * GST_NSECOND);
        bool isPlaying = (getStateResult == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PLAYING);

        if (m_readyState == MediaPlayer::HaveMetadata && oldReadyState > MediaPlayer::HaveMetadata && isPlaying) {
            printf("### %s: Changing pipeline to PAUSED...\n", __PRETTY_FUNCTION__); fflush(stdout);
            // Not using changePipelineState() because we don't want the state to drop to GST_STATE_NULL ever.
            bool ok = gst_element_set_state(m_pipeline.get(), GST_STATE_PAUSED) == GST_STATE_CHANGE_SUCCESS;
            printf("### %s: Changing pipeline to PAUSED: %s\n", __PRETTY_FUNCTION__, (ok)?"OK":"ERROR"); fflush(stdout);
        }
        m_player->readyStateChanged();
    }
}

void MediaPlayerPrivateGStreamerMSE::waitForSeekCompleted()
{
    if (!m_seeking)
        return;

    LOG_MEDIA_MESSAGE("Waiting for MSE seek completed");
    m_seekCompleted = false;
}

void MediaPlayerPrivateGStreamerMSE::seekCompleted()
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    if (m_seekCompleted)
        return;

    LOG_MEDIA_MESSAGE("MSE seek completed");
    m_seekCompleted = true;

    if (!seeking() && m_readyState >= MediaPlayer::HaveFutureData)
        gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);

    if (!m_seeking)
        m_player->timeChanged();
}

void MediaPlayerPrivateGStreamerMSE::setRate(float rate)
{
    notImplemented();
    return;

    // Higher rate causes crash.
    rate = clampTo(rate, -20.0, 20.0);

    // Avoid useless playback rate update.
    if (m_playbackRate == rate) {
        // and make sure that upper layers were notified if rate was set

        if (!m_changingRate && m_player->rate() != m_playbackRate)
            m_player->rateChanged();
        return;
    }

    if (isLiveStream()) {
        // notify upper layers that we cannot handle passed rate.
        m_changingRate = false;
        m_player->rateChanged();
        return;
    }

    GstState state;
    GstState pending;

    m_playbackRate = rate;
    m_changingRate = true;

    gst_element_get_state(m_pipeline.get(), &state, &pending, 0);

    if (!rate) {
        m_changingRate = false;
        m_playbackRatePause = true;
        if (state != GST_STATE_PAUSED && pending != GST_STATE_PAUSED)
            changePipelineState(GST_STATE_PAUSED);
        return;
    }

    if ((state != GST_STATE_PLAYING && state != GST_STATE_PAUSED)
        || (pending == GST_STATE_PAUSED))
        return;

    updatePlaybackRate();
}

double MediaPlayerPrivateGStreamerMSE::rate() const
{
    return m_playbackRate;
}

void MediaPlayerPrivateGStreamerMSE::setPreservesPitch(bool preservesPitch)
{
    m_preservesPitch = preservesPitch;
}

std::unique_ptr<PlatformTimeRanges> MediaPlayerPrivateGStreamerMSE::buffered() const
{
    return m_mediaSource->buffered();
}

static StreamType getStreamType(GstElement* element)
{
    g_return_val_if_fail(GST_IS_ELEMENT(element), Unknown);

    GstIterator* it;
    GValue item = G_VALUE_INIT;
    StreamType result = Unknown;

    it = gst_element_iterate_sink_pads(element);

    if (it && (gst_iterator_next(it, &item)) == GST_ITERATOR_OK) {
        GstPad* pad = GST_PAD(g_value_get_object(&item));
        if (pad) {
            GstCaps* caps = gst_pad_get_current_caps(pad);
            if (caps && GST_IS_CAPS(caps)) {
                const GstStructure* structure = gst_caps_get_structure(caps, 0);
                if (structure) {
                    const gchar* mediatype = gst_structure_get_name(structure);
                    if (mediatype) {
                        // Look for "audio", "video", "text"
                        switch (mediatype[0]) {
                        case 'a':
                            result = Audio;
                            break;
                        case 'v':
                            result = Video;
                            break;
                        case 't':
                            result = Text;
                            break;
                        default:
                            break;
                        }
                    }
                }
                gst_caps_unref(caps);
            }
        }
    }

    g_value_unset(&item);

    if (it)
        gst_iterator_free(it);

    return result;
}

void MediaPlayerPrivateGStreamerMSE::handleSyncMessage(GstMessage* message)
{
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ELEMENT:
        {
#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
            const GstStructure* structure = gst_message_get_structure(message);
            // Here we receive the DRM init data from the pipeline: we will emit
            // the needkey event with that data and the browser might create a
            // CDMSession from this event handler. If such a session was created
            // We will emit the message event from the session to provide the
            // DRM challenge to the browser and wait for an update. If on the
            // contrary no session was created we won't wait and let the pipeline
            // error out by itself.
            if (gst_structure_has_name(structure, "drm-key-needed")) {
                GstBuffer* data;
                const char* keySystemId;
                gboolean valid = gst_structure_get(structure, "data", GST_TYPE_BUFFER, &data,
                    "key-system-id", G_TYPE_STRING, &keySystemId, nullptr);
                GstMapInfo mapInfo;
                if (!valid || !gst_buffer_map(data, &mapInfo, GST_MAP_READ))
                    break;

                GST_DEBUG("queueing keyNeeded event");
                // We need to reset the semaphore first. signal, wait
                m_drmKeySemaphore.signal();
                m_drmKeySemaphore.wait();
                // Fire the need key event from main thread
                callOnMainThreadAndWait([&] {
                    // FIXME: Provide a somehow valid sessionId.
#if ENABLE(ENCRYPTED_MEDIA)
                        needKey(keySystemId, "sessionId", reinterpret_cast<const unsigned char *>(mapInfo.data), mapInfo.size);
#elif ENABLE(ENCRYPTED_MEDIA_V2)
                        RefPtr<Uint8Array> initData = Uint8Array::create(reinterpret_cast<const unsigned char *>(mapInfo.data), mapInfo.size);
                        needKey(initData);
#else
                        ASSERT_NOT_REACHED();
#endif
                    });
                // Wait for a potential license
                GST_DEBUG("waiting for a license");
                m_drmKeySemaphore.wait();
                GST_DEBUG("finished waiting");
                gst_buffer_unmap(data, &mapInfo);
            }
#endif
            break;
        }
        /* // This is now directly handled by MediaSourceClientGStreamerMSE
        case GST_MESSAGE_DURATION_CHANGED:
        {
            m_pendingAsyncOperationsLock.lock();
            guint asyncOperationId = g_timeout_add(0, (GSourceFunc)mediaPlayerPrivateNotifyDurationChanged, this);
            m_pendingAsyncOperations = g_list_append(m_pendingAsyncOperations, GUINT_TO_POINTER(asyncOperationId));
            m_pendingAsyncOperationsLock.unlock();
            break;
        }
        */
        default:
            break;
    }
}

gboolean MediaPlayerPrivateGStreamerMSE::handleMessage(GstMessage* message)
{
    GUniqueOutPtr<GError> err;
    GUniqueOutPtr<gchar> debug;
    MediaPlayer::NetworkState error;
    bool issueError = true;
    bool attemptNextLocation = false;
    const GstStructure* structure = gst_message_get_structure(message);
    GstState requestedState, currentState;

    m_canFallBackToLastFinishedSeekPosition = false;

    if (structure) {
        const gchar* messageTypeName = gst_structure_get_name(structure);

        // Redirect messages are sent from elements, like qtdemux, to
        // notify of the new location(s) of the media.
        if (!g_strcmp0(messageTypeName, "redirect")) {
            mediaLocationChanged(message);
            return TRUE;
        }
    }

    // We ignore state changes from internal elements. They are forwarded to playbin2 anyway.
    bool messageSourceIsPlaybin = GST_MESSAGE_SRC(message) == reinterpret_cast<GstObject*>(m_pipeline.get());

    LOG_MEDIA_MESSAGE("Message %s received from element %s", GST_MESSAGE_TYPE_NAME(message), GST_MESSAGE_SRC_NAME(message));
    printf("### %s: element: %s, message: %s\n", __PRETTY_FUNCTION__, GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message)); fflush(stdout);

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        if (m_resetPipeline)
            break;
        if (m_missingPluginsCallback)
            break;
        gst_message_parse_error(message, &err.outPtr(), &debug.outPtr());
        ERROR_MEDIA_MESSAGE("Error %d: %s (url=%s)", err->code, err->message, m_url.string().utf8().data());

        printf("### %s: Error %d: %s (url=%s)", __PRETTY_FUNCTION__, err->code, err->message, m_url.string().utf8().data()); fflush(stdout);

        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, "webkit-video.error");

        error = MediaPlayer::Empty;
        if (err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND
            || err->code == GST_STREAM_ERROR_WRONG_TYPE
            || err->code == GST_STREAM_ERROR_FAILED
            || err->code == GST_CORE_ERROR_MISSING_PLUGIN
            || err->code == GST_RESOURCE_ERROR_NOT_FOUND)
            error = MediaPlayer::FormatError;
        else if (err->domain == GST_STREAM_ERROR) {
            // Let the mediaPlayerClient handle the stream error, in
            // this case the HTMLMediaElement will emit a stalled
            // event.
            if (err->code == GST_STREAM_ERROR_TYPE_NOT_FOUND) {
                ERROR_MEDIA_MESSAGE("Decode error, let the Media element emit a stalled event.");
                break;
            }
            error = MediaPlayer::DecodeError;
            attemptNextLocation = true;
        } else if (err->domain == GST_RESOURCE_ERROR)
            error = MediaPlayer::NetworkError;

        if (attemptNextLocation)
            issueError = !loadNextLocation();
        if (issueError)
            loadingFailed(error);
        break;
    case GST_MESSAGE_EOS:
        didEnd();
        break;
    case GST_MESSAGE_ASYNC_DONE:
        if (!messageSourceIsPlaybin || m_delayingLoad)
            break;
        asyncStateChangeDone();
        break;
    case GST_MESSAGE_STATE_CHANGED: {
        // DEBUG
        {
            GstState newState;
            gst_message_parse_state_changed(message, &currentState, &newState, 0);
            printf("### %s: State changed %s --> %s\n", __PRETTY_FUNCTION__, gst_element_state_get_name(currentState), gst_element_state_get_name(newState)); fflush(stdout);
        }
        if (!messageSourceIsPlaybin || m_delayingLoad) {
            printf("### %s: messageSourceIsPlaybin=%s, m_delayingLoad=%s\n", __PRETTY_FUNCTION__, messageSourceIsPlaybin?"true":"false", m_delayingLoad?"true":"false"); fflush(stdout);
            break;
        }
        updateStates();

        // Construct a filename for the graphviz dot file output.
        GstState newState;
        gst_message_parse_state_changed(message, &currentState, &newState, 0);
        CString dotFileName = String::format("webkit-video.%s_%s", gst_element_state_get_name(currentState), gst_element_state_get_name(newState)).utf8();
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.data());

        printf("### %s: Playbin changed %s --> %s\n", __PRETTY_FUNCTION__, gst_element_state_get_name(currentState), gst_element_state_get_name(newState)); fflush(stdout);

        break;
    }
    case GST_MESSAGE_BUFFERING:
        processBufferingStats(message);
        break;
/* // This is now directly handled by MediaSourceClientGStreamerMSE
    case GST_MESSAGE_DURATION_CHANGED:
        durationChanged();
        break;
*/
    case GST_MESSAGE_REQUEST_STATE:
        gst_message_parse_request_state(message, &requestedState);
        gst_element_get_state(m_pipeline.get(), &currentState, nullptr, 250 * GST_NSECOND);
        if (requestedState < currentState) {
            GUniquePtr<gchar> elementName(gst_element_get_name(GST_ELEMENT(message)));
            INFO_MEDIA_MESSAGE("Element %s requested state change to %s", elementName.get(),
                gst_element_state_get_name(requestedState));
            m_requestedState = requestedState;
            if (!changePipelineState(requestedState))
                loadingFailed(MediaPlayer::Empty);
        }
        break;
    case GST_MESSAGE_CLOCK_LOST:
        // This can only happen in PLAYING state and we should just
        // get a new clock by moving back to PAUSED and then to
        // PLAYING again.
        // This can happen if the stream that ends in a sink that
        // provides the current clock disappears, for example if
        // the audio sink provides the clock and the audio stream
        // is disabled. It also happens relatively often with
        // HTTP adaptive streams when switching between different
        // variants of a stream.
        gst_element_set_state(m_pipeline.get(), GST_STATE_PAUSED);
        gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
        break;
    case GST_MESSAGE_LATENCY:
        // Recalculate the latency, we don't need any special handling
        // here other than the GStreamer default.
        // This can happen if the latency of live elements changes, or
        // for one reason or another a new live element is added or
        // removed from the pipeline.
        gst_bin_recalculate_latency(GST_BIN(m_pipeline.get()));
        break;
    case GST_MESSAGE_ELEMENT:
        if (gst_is_missing_plugin_message(message)) {
            if (gst_install_plugins_supported()) {
                m_missingPluginsCallback = MediaPlayerRequestInstallMissingPluginsCallback::create([this](uint32_t result) {
                    m_missingPluginsCallback = nullptr;
                    if (result != GST_INSTALL_PLUGINS_SUCCESS)
                        return;

                    changePipelineState(GST_STATE_READY);
                    changePipelineState(GST_STATE_PAUSED);
                });
                GUniquePtr<char> detail(gst_missing_plugin_message_get_installer_detail(message));
                GUniquePtr<char> description(gst_missing_plugin_message_get_description(message));
                m_player->client().requestInstallMissingPlugins(String::fromUTF8(detail.get()), String::fromUTF8(description.get()), *m_missingPluginsCallback);
            }
        }
#if USE(GSTREAMER_MPEGTS)
        else if (GstMpegtsSection* section = gst_message_parse_mpegts_section(message)) {
            processMpegTsSection(section);
            gst_mpegts_section_unref(section);
        }

#endif
        else {
            const GstStructure* structure = gst_message_get_structure(message);
            if (gst_structure_has_name(structure, "adaptive-streaming-statistics") && gst_structure_has_field(structure, "fragment-download-time")) {
                GUniqueOutPtr<gchar> uri;
                GstClockTime time;
                gst_structure_get(structure, "uri", G_TYPE_STRING, &uri.outPtr(), "fragment-download-time", GST_TYPE_CLOCK_TIME, &time, nullptr);
                INFO_MEDIA_MESSAGE("Fragment %s download time %" GST_TIME_FORMAT, uri.get(), GST_TIME_ARGS(time));
            }
        }
        break;
    case GST_MESSAGE_TOC:
        processTableOfContents(message);
        break;
/*
    case GST_MESSAGE_RESET_TIME:
        if (m_source && WEBKIT_IS_MEDIA_SRC(m_source.get())) {
            StreamType streamType = getStreamType(GST_ELEMENT(GST_MESSAGE_SRC(message)));
            if (streamType == Audio || streamType == Video)
                webkit_media_src_segment_needed(WEBKIT_MEDIA_SRC(m_source.get()), streamType);
        }
        break;
*/
    default:
        LOG_MEDIA_MESSAGE("Unhandled GStreamer message type: %s",
                    GST_MESSAGE_TYPE_NAME(message));
        break;
    }
    return TRUE;
}

void MediaPlayerPrivateGStreamerMSE::processBufferingStats(GstMessage* message)
{
    m_buffering = true;
    gst_message_parse_buffering(message, &m_bufferingPercentage);

    LOG_MEDIA_MESSAGE("[Buffering] Buffering: %d%%.", m_bufferingPercentage);

    updateStates();
}

#if USE(GSTREAMER_MPEGTS)
void MediaPlayerPrivateGStreamerMSE::processMpegTsSection(GstMpegtsSection* section)
{
    ASSERT(section);

    if (section->section_type == GST_MPEGTS_SECTION_PMT) {
        const GstMpegtsPMT* pmt = gst_mpegts_section_get_pmt(section);
        m_metadataTracks.clear();
        for (guint i = 0; i < pmt->streams->len; ++i) {
            const GstMpegtsPMTStream* stream = static_cast<const GstMpegtsPMTStream*>(g_ptr_array_index(pmt->streams, i));
            if (stream->stream_type == 0x05 || stream->stream_type >= 0x80) {
                AtomicString pid = String::number(stream->pid);
                RefPtr<InbandMetadataTextTrackPrivateGStreamer> track = InbandMetadataTextTrackPrivateGStreamer::create(
                    InbandTextTrackPrivate::Metadata, InbandTextTrackPrivate::Data, pid);

                // 4.7.10.12.2 Sourcing in-band text tracks
                // If the new text track's kind is metadata, then set the text track in-band metadata track dispatch
                // type as follows, based on the type of the media resource:
                // Let stream type be the value of the "stream_type" field describing the text track's type in the
                // file's program map section, interpreted as an 8-bit unsigned integer. Let length be the value of
                // the "ES_info_length" field for the track in the same part of the program map section, interpreted
                // as an integer as defined by the MPEG-2 specification. Let descriptor bytes be the length bytes
                // following the "ES_info_length" field. The text track in-band metadata track dispatch type must be
                // set to the concatenation of the stream type byte and the zero or more descriptor bytes bytes,
                // expressed in hexadecimal using uppercase ASCII hex digits.
                String inbandMetadataTrackDispatchType;
                appendUnsignedAsHexFixedSize(stream->stream_type, inbandMetadataTrackDispatchType, 2);
                for (guint j = 0; j < stream->descriptors->len; ++j) {
                    const GstMpegtsDescriptor* descriptor = static_cast<const GstMpegtsDescriptor*>(g_ptr_array_index(stream->descriptors, j));
                    for (guint k = 0; k < descriptor->length; ++k)
                        appendByteAsHex(descriptor->data[k], inbandMetadataTrackDispatchType);
                }
                track->setInBandMetadataTrackDispatchType(inbandMetadataTrackDispatchType);

                m_metadataTracks.add(pid, track);
                m_player->addTextTrack(track);
            }
        }
    } else {
        AtomicString pid = String::number(section->pid);
        RefPtr<InbandMetadataTextTrackPrivateGStreamer> track = m_metadataTracks.get(pid);
        if (!track)
            return;

        GRefPtr<GBytes> data = gst_mpegts_section_get_data(section);
        gsize size;
        const void* bytes = g_bytes_get_data(data.get(), &size);

        track->addDataCue(MediaTime::createWithDouble(currentTimeDouble()), MediaTime::createWithDouble(currentTimeDouble()), bytes, size);
    }
}
#endif

void MediaPlayerPrivateGStreamerMSE::processTableOfContents(GstMessage* message)
{
    if (m_chaptersTrack)
        m_player->removeTextTrack(m_chaptersTrack);

    m_chaptersTrack = InbandMetadataTextTrackPrivateGStreamer::create(InbandTextTrackPrivate::Chapters, InbandTextTrackPrivate::Generic);
    m_player->addTextTrack(m_chaptersTrack);

    GRefPtr<GstToc> toc;
    gboolean updated;
    gst_message_parse_toc(message, &toc.outPtr(), &updated);
    ASSERT(toc);

    for (GList* i = gst_toc_get_entries(toc.get()); i; i = i->next)
        processTableOfContentsEntry(static_cast<GstTocEntry*>(i->data), 0);
}

void MediaPlayerPrivateGStreamerMSE::processTableOfContentsEntry(GstTocEntry* entry, GstTocEntry* parent)
{
    UNUSED_PARAM(parent);
    ASSERT(entry);

    RefPtr<GenericCueData> cue = GenericCueData::create();

    gint64 start = -1, stop = -1;
    gst_toc_entry_get_start_stop_times(entry, &start, &stop);
    if (start != -1)
        cue->setStartTime(MediaTime(start, GST_SECOND));
    if (stop != -1)
        cue->setEndTime(MediaTime(stop, GST_SECOND));

    GstTagList* tags = gst_toc_entry_get_tags(entry);
    if (tags) {
        gchar* title =  0;
        gst_tag_list_get_string(tags, GST_TAG_TITLE, &title);
        if (title) {
            cue->setContent(title);
            g_free(title);
        }
    }

    m_chaptersTrack->addGenericCue(cue.release());

    for (GList* i = gst_toc_entry_get_sub_entries(entry); i; i = i->next)
        processTableOfContentsEntry(static_cast<GstTocEntry*>(i->data), entry);
}

void MediaPlayerPrivateGStreamerMSE::fillTimerFired()
{
    GstQuery* query = gst_query_new_buffering(GST_FORMAT_PERCENT);

    if (!gst_element_query(m_pipeline.get(), query)) {
        gst_query_unref(query);
        return;
    }

    gint64 start, stop;
    gdouble fillStatus = 100.0;

    gst_query_parse_buffering_range(query, 0, &start, &stop, 0);
    gst_query_unref(query);

    if (stop != -1)
        fillStatus = 100.0 * stop / GST_FORMAT_PERCENT_MAX;

    LOG_MEDIA_MESSAGE("[Buffering] Download buffer filled up to %f%%", fillStatus);

    if (!m_mediaDuration)
        durationChanged();

    // Update maxTimeLoaded only if the media duration is
    // available. Otherwise we can't compute it.
    if (m_mediaDuration) {
        if (fillStatus == 100.0)
            m_maxTimeLoaded = m_mediaDuration;
        else
            m_maxTimeLoaded = static_cast<float>((fillStatus * m_mediaDuration) / 100.0);
        LOG_MEDIA_MESSAGE("[Buffering] Updated maxTimeLoaded: %f", m_maxTimeLoaded);
    }

    m_downloadFinished = fillStatus == 100.0;
    if (!m_downloadFinished) {
        updateStates();
        return;
    }

    // Media is now fully loaded. It will play even if network
    // connection is cut. Buffering is done, remove the fill source
    // from the main loop.
    m_fillTimer.stop();
    updateStates();
}

float MediaPlayerPrivateGStreamerMSE::maxTimeSeekable() const
{
    if (m_errorOccured)
        return 0.0f;

    LOG_MEDIA_MESSAGE("maxTimeSeekable");
    // infinite duration means live stream
    if (std::isinf(duration()))
        return 0.0f;

    return duration();
}

float MediaPlayerPrivateGStreamerMSE::maxTimeLoaded() const
{
    if (m_errorOccured)
        return 0.0f;

    float loaded = m_maxTimeLoaded;
    if (m_isEndReached && m_mediaDuration)
        loaded = m_mediaDuration;
    LOG_MEDIA_MESSAGE("maxTimeLoaded: %f", loaded);
    return loaded;
}

bool MediaPlayerPrivateGStreamerMSE::didLoadingProgress() const
{
    if (!m_pipeline ||
            (!isMediaSource() && !totalBytes())
            || !m_mediaDuration)
        return false;
    float currentMaxTimeLoaded = maxTimeLoaded();
    bool didLoadingProgress = currentMaxTimeLoaded != m_maxTimeLoadedAtLastDidLoadingProgress;
    m_maxTimeLoadedAtLastDidLoadingProgress = currentMaxTimeLoaded;
    LOG_MEDIA_MESSAGE("didLoadingProgress: %d", didLoadingProgress);
    return didLoadingProgress;
}

unsigned long long MediaPlayerPrivateGStreamerMSE::totalBytes() const
{
    if (m_errorOccured)
        return 0;

    if (m_totalBytes)
        return m_totalBytes;

    if (!m_source)
        return 0;

    GstFormat fmt = GST_FORMAT_BYTES;
    gint64 length = 0;
    if (gst_element_query_duration(m_source.get(), fmt, &length)) {
        INFO_MEDIA_MESSAGE("totalBytes %" G_GINT64_FORMAT, length);
        m_totalBytes = static_cast<unsigned long long>(length);
        m_isStreaming = !length;
        return m_totalBytes;
    }

    // Fall back to querying the source pads manually.
    // See also https://bugzilla.gnome.org/show_bug.cgi?id=638749
    GstIterator* iter = gst_element_iterate_src_pads(m_source.get());
    bool done = false;
    while (!done) {
        GValue item = G_VALUE_INIT;
        switch (gst_iterator_next(iter, &item)) {
        case GST_ITERATOR_OK: {
            GstPad* pad = static_cast<GstPad*>(g_value_get_object(&item));
            gint64 padLength = 0;
            if (gst_pad_query_duration(pad, fmt, &padLength) && padLength > length)
                length = padLength;
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iter);
            break;
        case GST_ITERATOR_ERROR:
            FALLTHROUGH;
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }

        g_value_unset(&item);
    }

    gst_iterator_free(iter);

    INFO_MEDIA_MESSAGE("totalBytes %" G_GINT64_FORMAT, length);
    m_totalBytes = static_cast<unsigned long long>(length);
    m_isStreaming = !length;
    return m_totalBytes;
}

void MediaPlayerPrivateGStreamerMSE::sourceChanged()
{
    m_source.clear();
    g_object_get(m_pipeline.get(), "source", &m_source.outPtr(), nullptr);

    g_assert(WEBKIT_IS_MEDIA_SRC(m_source.get()));

    m_playbackPipeline->setWebKitMediaSrc(WEBKIT_MEDIA_SRC(m_source.get()));

    MediaSourceGStreamer::open(m_mediaSource.get(), RefPtr<MediaPlayerPrivateGStreamerMSE>(this));
    g_signal_connect(m_source.get(), "video-changed", G_CALLBACK(mediaPlayerPrivateVideoChangedCallback), this);
    g_signal_connect(m_source.get(), "audio-changed", G_CALLBACK(mediaPlayerPrivateAudioChangedCallback), this);
    g_signal_connect(m_source.get(), "text-changed", G_CALLBACK(mediaPlayerPrivateTextChangedCallback), this);
    webkit_media_src_set_mediaplayerprivate(WEBKIT_MEDIA_SRC(m_source.get()), this);
}

void MediaPlayerPrivateGStreamerMSE::cancelLoad()
{
#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    // Potentially unblock GStreamer thread for DRM license acquisition.
    m_drmKeySemaphore.signal();
#endif

    if (m_networkState < MediaPlayer::Loading || m_networkState == MediaPlayer::Loaded)
        return;

    if (m_pipeline)
        changePipelineState(GST_STATE_READY);
}

void MediaPlayerPrivateGStreamerMSE::asyncStateChangeDone()
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    if (!m_pipeline || m_errorOccured)
        return;

    if (m_seeking) {
        if (m_seekIsPending)
            updateStates();
        else {
            LOG_MEDIA_MESSAGE("[Seek] seeked to %f", m_seekTime);
            m_seeking = false;
            printf("### %s m_seeking=%s\n", __PRETTY_FUNCTION__, m_seeking?"true":"false"); fflush(stdout);
            m_cachedPosition = -1;
            if (m_timeOfOverlappingSeek != m_seekTime && m_timeOfOverlappingSeek != -1) {
                seek(m_timeOfOverlappingSeek);
                m_timeOfOverlappingSeek = -1;
                return;
            }
            m_timeOfOverlappingSeek = -1;

            // The pipeline can still have a pending state. In this case a position query will fail.
            // Right now we can use m_seekTime as a fallback.
            m_canFallBackToLastFinishedSeekPosition = true;
            timeChanged();
        }
    } else
        updateStates();
}

void MediaPlayerPrivateGStreamerMSE::updateStates()
{
    if (!m_pipeline)
        return;

    if (m_errorOccured)
        return;

    MediaPlayer::NetworkState oldNetworkState = m_networkState;
    MediaPlayer::ReadyState oldReadyState = m_readyState;
    GstState state;
    GstState pending;

    GstStateChangeReturn getStateResult = gst_element_get_state(m_pipeline.get(), &state, &pending, 250 * GST_NSECOND);

    bool shouldUpdatePlaybackState = false;
    switch (getStateResult) {
    case GST_STATE_CHANGE_SUCCESS: {
        LOG_MEDIA_MESSAGE("State: %s, pending: %s", gst_element_state_get_name(state), gst_element_state_get_name(pending));

        // Do nothing if on EOS and state changed to READY to avoid recreating the player
        // on HTMLMediaElement and properly generate the video 'ended' event.
        if (m_isEndReached && state == GST_STATE_READY)
            break;

        if (state <= GST_STATE_READY) {
            m_resetPipeline = true;
            m_mediaDuration = 0;
        } else {
            m_resetPipeline = false;
            cacheDuration();
        }

        bool didBuffering = m_buffering;

        // Update ready and network states.
        switch (state) {
        case GST_STATE_NULL:
            m_readyState = MediaPlayer::HaveNothing;
            printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
            m_networkState = MediaPlayer::Empty;
            break;
        case GST_STATE_READY:
            m_readyState = MediaPlayer::HaveMetadata;
            printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
            m_networkState = MediaPlayer::Empty;
            break;
        case GST_STATE_PAUSED:
        case GST_STATE_PLAYING:
            if (isMediaSource() && m_seeking && !timeIsBuffered(m_seekTime)) {
                m_readyState = MediaPlayer::HaveMetadata;
                // TODO: NetworkState?
                printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
            } else if (m_buffering) {
                if (m_bufferingPercentage == 100) {
                    LOG_MEDIA_MESSAGE("[Buffering] Complete.");
                    m_buffering = false;
                    m_readyState = MediaPlayer::HaveEnoughData;
                    printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
                    m_networkState = m_downloadFinished ? MediaPlayer::Idle : MediaPlayer::Loading;
                } else {
                    m_readyState = MediaPlayer::HaveCurrentData;
                    printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
                    m_networkState = MediaPlayer::Loading;
                }
            } else if (m_downloadFinished) {
                m_readyState = MediaPlayer::HaveEnoughData;
                printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
                m_networkState = MediaPlayer::Loaded;
            } else {
                m_readyState = MediaPlayer::HaveFutureData;
                printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
                m_networkState = MediaPlayer::Loading;
            }

            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }

        // Sync states where needed.
        if (state == GST_STATE_PAUSED) {
            if (!m_volumeAndMuteInitialized) {
                notifyPlayerOfVolumeChange();
                notifyPlayerOfMute();
                m_volumeAndMuteInitialized = true;
            }

            if (didBuffering && !m_buffering && !m_paused && m_playbackRate) {
                LOG_MEDIA_MESSAGE("[Buffering] Restarting playback.");
                changePipelineState(GST_STATE_PLAYING);
            }
        } else if (state == GST_STATE_PLAYING) {
            m_paused = false;

            if ((m_buffering && !isLiveStream()) || !m_playbackRate) {
                LOG_MEDIA_MESSAGE("[Buffering] Pausing stream for buffering.");
                changePipelineState(GST_STATE_PAUSED);
            }
        } else
            m_paused = true;

        if (m_requestedState == GST_STATE_PAUSED && state == GST_STATE_PAUSED) {
            shouldUpdatePlaybackState = true;
            LOG_MEDIA_MESSAGE("Requested state change to %s was completed", gst_element_state_get_name(state));
        }

        break;
    }
    case GST_STATE_CHANGE_ASYNC:
        LOG_MEDIA_MESSAGE("Async: State: %s, pending: %s", gst_element_state_get_name(state), gst_element_state_get_name(pending));
        // Change in progress.
        break;
    case GST_STATE_CHANGE_FAILURE:
        LOG_MEDIA_MESSAGE("Failure: State: %s, pending: %s", gst_element_state_get_name(state), gst_element_state_get_name(pending));
        // Change failed
        return;
    case GST_STATE_CHANGE_NO_PREROLL:
        LOG_MEDIA_MESSAGE("No preroll: State: %s, pending: %s", gst_element_state_get_name(state), gst_element_state_get_name(pending));

        // Live pipelines go in PAUSED without prerolling.
        m_isStreaming = true;
        setDownloadBuffering();

        if (state == GST_STATE_READY) {
            m_readyState = MediaPlayer::HaveNothing;
            printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
        } else if (state == GST_STATE_PAUSED) {
            m_readyState = MediaPlayer::HaveEnoughData;
            printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
            m_paused = true;
        } else if (state == GST_STATE_PLAYING)
            m_paused = false;

        if (!m_paused && m_playbackRate)
            changePipelineState(GST_STATE_PLAYING);

        m_networkState = MediaPlayer::Loading;
        break;
    default:
        LOG_MEDIA_MESSAGE("Else : %d", getStateResult);
        break;
    }

    m_requestedState = GST_STATE_VOID_PENDING;

    if (shouldUpdatePlaybackState)
        m_player->playbackStateChanged();

    if (m_networkState != oldNetworkState) {
        LOG_MEDIA_MESSAGE("Network State Changed from %u to %u", oldNetworkState, m_networkState);
        m_player->networkStateChanged();
    }
    if (m_readyState != oldReadyState) {
        LOG_MEDIA_MESSAGE("Ready State Changed from %u to %u", oldReadyState, m_readyState);
        m_player->readyStateChanged();
    }

    if (getStateResult == GST_STATE_CHANGE_SUCCESS && state >= GST_STATE_PAUSED) {
        updatePlaybackRate();
        if (m_seekIsPending
            && (!isMediaSource()
                || (isMediaSource() && timeIsBuffered(m_seekTime)))
            ) {
            LOG_MEDIA_MESSAGE("[Seek] committing pending seek to %f", m_seekTime);
            m_seekIsPending = false;
            m_seeking = doSeek(toGstClockTime(m_seekTime), m_player->rate(), static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE));
            printf("### %s m_seeking=%s\n", __PRETTY_FUNCTION__, m_seeking?"true":"false"); fflush(stdout);
            if (!m_seeking) {
                m_cachedPosition = -1;
                LOG_MEDIA_MESSAGE("[Seek] seeking to %f failed", m_seekTime);
            }
        }
    }
}

GRefPtr<GstCaps> MediaPlayerPrivateGStreamerMSE::currentDemuxerCaps() const
{
    GRefPtr<GstCaps> result;
    if (m_mediaSource && WEBKIT_IS_MEDIA_SRC(m_source.get())) {
        // METRO FIXME: Select the current demuxer pad (how to know?) instead of the first one
        GstPad* demuxersrcpad = webkit_media_src_get_video_pad(WEBKIT_MEDIA_SRC(m_source.get()), 0);

        if (demuxersrcpad) {
            result = adoptGRef(gst_pad_get_current_caps(demuxersrcpad));
        }
    }
    return result;
}

bool MediaPlayerPrivateGStreamerMSE::timeIsBuffered(float time)
{
    bool result = isMediaSource() && m_mediaSource->buffered()->contain(MediaTime::createWithFloat(time));
    LOG_MEDIA_MESSAGE("Time %f buffered? %s", time, result ? "aye" : "nope");
    return result;
}

void MediaPlayerPrivateGStreamerMSE::setMediaSourceClient(PassRefPtr<MediaSourceClientGStreamerMSE> mediaSourceClient)
{
    m_mediaSourceClient = mediaSourceClient;
}

RefPtr<MediaSourceClientGStreamerMSE> MediaPlayerPrivateGStreamerMSE::mediaSourceClient()
{
    return m_mediaSourceClient;
}

RefPtr<AppendPipeline> MediaPlayerPrivateGStreamerMSE::appendPipelineByTrackId(const AtomicString& trackId)
{
    if (trackId == AtomicString()) {
        printf("### %s: trackId is empty\n", __PRETTY_FUNCTION__); fflush(stdout);
    }

    ASSERT(!(trackId.isNull() || trackId.isEmpty()));

    for (HashMap<RefPtr<SourceBufferPrivateGStreamer>, RefPtr<AppendPipeline> >::iterator it = m_appendPipelinesMap.begin(); it != m_appendPipelinesMap.end(); ++it)
        if (it->value->trackId() == trackId)
            return it->value;

    return RefPtr<AppendPipeline>(0);
}


void MediaPlayerPrivateGStreamerMSE::mediaLocationChanged(GstMessage* message)
{
    if (m_mediaLocations)
        gst_structure_free(m_mediaLocations);

    const GstStructure* structure = gst_message_get_structure(message);
    if (structure) {
        // This structure can contain:
        // - both a new-location string and embedded locations structure
        // - or only a new-location string.
        m_mediaLocations = gst_structure_copy(structure);
        const GValue* locations = gst_structure_get_value(m_mediaLocations, "locations");

        if (locations)
            m_mediaLocationCurrentIndex = static_cast<int>(gst_value_list_get_size(locations)) -1;

        loadNextLocation();
    }
}

bool MediaPlayerPrivateGStreamerMSE::loadNextLocation()
{
    if (!m_mediaLocations)
        return false;

    const GValue* locations = gst_structure_get_value(m_mediaLocations, "locations");
    const gchar* newLocation = 0;

    if (!locations) {
        // Fallback on new-location string.
        newLocation = gst_structure_get_string(m_mediaLocations, "new-location");
        if (!newLocation)
            return false;
    }

    if (!newLocation) {
        if (m_mediaLocationCurrentIndex < 0) {
            m_mediaLocations = 0;
            return false;
        }

        const GValue* location = gst_value_list_get_value(locations,
                                                          m_mediaLocationCurrentIndex);
        const GstStructure* structure = gst_value_get_structure(location);

        if (!structure) {
            m_mediaLocationCurrentIndex--;
            return false;
        }

        newLocation = gst_structure_get_string(structure, "new-location");
    }

    if (newLocation) {
        // Found a candidate. new-location is not always an absolute url
        // though. We need to take the base of the current url and
        // append the value of new-location to it.
        URL baseUrl = gst_uri_is_valid(newLocation) ? URL() : m_url;
        URL newUrl = URL(baseUrl, newLocation);

        RefPtr<SecurityOrigin> securityOrigin = SecurityOrigin::create(m_url);
        if (securityOrigin->canRequest(newUrl)) {
            INFO_MEDIA_MESSAGE("New media url: %s", newUrl.string().utf8().data());

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
            // Potentially unblock GStreamer thread for DRM license acquisition.
            m_drmKeySemaphore.signal();
#endif

            // Reset player states.
            m_networkState = MediaPlayer::Loading;
            m_player->networkStateChanged();
            m_readyState = MediaPlayer::HaveNothing;
            printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
            m_player->readyStateChanged();

            // Reset pipeline state.
            m_resetPipeline = true;
            changePipelineState(GST_STATE_READY);

            GstState state;
            gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);
            if (state <= GST_STATE_READY) {
                // Set the new uri and start playing.
                g_object_set(m_pipeline.get(), "uri", newUrl.string().utf8().data(), nullptr);
                m_url = newUrl;
                changePipelineState(GST_STATE_PLAYING);
                return true;
            }
        } else
            INFO_MEDIA_MESSAGE("Not allowed to load new media location: %s", newUrl.string().utf8().data());
    }
    m_mediaLocationCurrentIndex--;
    return false;
}

void MediaPlayerPrivateGStreamerMSE::loadStateChanged()
{
    updateStates();
}

void MediaPlayerPrivateGStreamerMSE::timeChanged()
{
    updateStates();
    m_player->timeChanged();
}

void MediaPlayerPrivateGStreamerMSE::didEnd()
{
    // Synchronize position and duration values to not confuse the
    // HTMLMediaElement. In some cases like reverse playback the
    // position is not always reported as 0 for instance.
    float now = currentTime();
    if (now > 0 && now <= duration() && m_mediaDuration != now) {
        m_mediaDurationKnown = true;
        m_mediaDuration = now;
        m_player->durationChanged();
    }

    m_isEndReached = true;
    timeChanged();

    if (!m_player->client().mediaPlayerIsLooping()) {
        m_paused = true;
        changePipelineState(GST_STATE_READY);
        m_downloadFinished = false;
    }
}

void MediaPlayerPrivateGStreamerMSE::cacheDuration()
{
    if (m_mediaDuration || !m_mediaDurationKnown)
        return;

    float newDuration = duration();
    if (std::isinf(newDuration)) {
        // Only pretend that duration is not available if the the query failed in a stable pipeline state.
        GstState state;
        if (gst_element_get_state(m_pipeline.get(), &state, nullptr, 0) == GST_STATE_CHANGE_SUCCESS && state > GST_STATE_READY)
            m_mediaDurationKnown = false;
        return;
    }

    m_mediaDuration = newDuration;
}

void MediaPlayerPrivateGStreamerMSE::notifyDurationChanged()
{
    m_pendingAsyncOperationsLock.lock();
    ASSERT(m_pendingAsyncOperations);
    m_pendingAsyncOperations = g_list_remove(m_pendingAsyncOperations, m_pendingAsyncOperations->data);
    m_pendingAsyncOperationsLock.unlock();

    durationChanged();
}

void MediaPlayerPrivateGStreamerMSE::durationChanged()
{
    float previousDuration = m_mediaDuration;

    if (!m_mediaSourceClient) {
        printf("### %s: m_mediaSourceClient is null, not doing anything\n", __PRETTY_FUNCTION__); fflush(stdout);
        return;
    }
    m_mediaDuration = m_mediaSourceClient->duration().toFloat();

    printf("### %s: previous=%f, new=%f\n", __PRETTY_FUNCTION__, previousDuration, m_mediaDuration); fflush(stdout);
    cacheDuration();
    // Avoid emiting durationchanged in the case where the previous
    // duration was 0 because that case is already handled by the
    // HTMLMediaElement.
    if (/*previousDuration &&*/ m_mediaDuration != previousDuration) {
        printf("### %s: Notifying player\n", __PRETTY_FUNCTION__); fflush(stdout);
        m_player->durationChanged();
    }
}

void MediaPlayerPrivateGStreamerMSE::loadingFailed(MediaPlayer::NetworkState error)
{
    m_errorOccured = true;
    if (m_networkState != error) {
        m_networkState = error;
        m_player->networkStateChanged();
    }
    if (m_readyState != MediaPlayer::HaveNothing) {
        m_readyState = MediaPlayer::HaveNothing;
        printf("### %s: m_readyState=%s\n", __PRETTY_FUNCTION__, dumpReadyState(m_readyState)); fflush(stdout);
        m_player->readyStateChanged();
    }

    // Loading failed, remove ready timer.
    m_readyTimerHandler.cancel();
}

static HashSet<String> mimeTypeCache()
{
    initializeGStreamerAndRegisterWebKitElements();

    DEPRECATED_DEFINE_STATIC_LOCAL(HashSet<String>, cache, ());
    static bool typeListInitialized = false;

    if (typeListInitialized)
        return cache;

    const char* mimeTypes[] = {
        "audio/mp4",
        "video/mp4"
    };

    for (unsigned i = 0; i < (sizeof(mimeTypes) / sizeof(*mimeTypes)); ++i)
        cache.add(String(mimeTypes[i]));

    typeListInitialized = true;
    return cache;
}

void MediaPlayerPrivateGStreamerMSE::getSupportedTypes(HashSet<String>& types)
{
    types = mimeTypeCache();
}

bool MediaPlayerPrivateGStreamerMSE::supportsKeySystem(const String& keySystem, const String& mimeType)
{
    GST_DEBUG("Checking for KeySystem support with %s and type %s", keySystem.utf8().data(), mimeType.utf8().data());

#if ENABLE(ENCRYPTED_MEDIA)
    if (equalIgnoringCase(keySystem, "org.w3.clearkey"))
        return true;
#endif

#if USE(DXDRM) && (ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2))
    if (equalIgnoringCase(keySystem, "com.microsoft.playready")
        || equalIgnoringCase(keySystem, "com.youtube.playready"))
        return true;
#endif

    return false;
}

#if ENABLE(ENCRYPTED_MEDIA)
MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerMSE::addKey(const String& keySystem, const unsigned char* keyData, unsigned keyLength, const unsigned char* /* initData */, unsigned /* initDataLength */ , const String& sessionID)
{
    LOG_MEDIA_MESSAGE("addKey system: %s, length: %u, session: %s", keySystem.utf8().data(), keyLength, sessionID.utf8().data());

#if USE(DXDRM)
    if (equalIgnoringCase(keySystem, "com.microsoft.playready")
        || equalIgnoringCase(keySystem, "com.youtube.playready")) {
        RefPtr<Uint8Array> key = Uint8Array::create(keyData, keyLength);
        RefPtr<Uint8Array> nextMessage;
        unsigned short errorCode;
        unsigned long systemCode;

        bool result = m_dxdrmSession->dxdrmProcessKey(key.get(), nextMessage, errorCode, systemCode);
        if (errorCode || !result) {
            LOG_MEDIA_MESSAGE("Error processing key: errorCode: %u, result: %d", errorCode, result);
            m_drmKeySemaphore.signal();
            return MediaPlayer::InvalidPlayerState;
        }

        gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
            gst_structure_new("dxdrm-session", "session", G_TYPE_POINTER, m_dxdrmSession, nullptr)));
        m_drmKeySemaphore.signal();
        return MediaPlayer::NoError;
    }
#endif

    if (!equalIgnoringCase(keySystem, "org.w3.clearkey"))
        return MediaPlayer::KeySystemNotSupported;

    GstBuffer* buffer = gst_buffer_new_wrapped(g_memdup(keyData, keyLength), keyLength);
    gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
        gst_structure_new("drm-cipher", "key", GST_TYPE_BUFFER, buffer, nullptr)));
    gst_buffer_unref(buffer);
    m_drmKeySemaphore.signal();
    return MediaPlayer::NoError;
}

MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerMSE::generateKeyRequest(const String& keySystem, const unsigned char* initDataPtr, unsigned initDataLength)
{
    LOG_MEDIA_MESSAGE("generating key request for system: %s", keySystem.utf8().data());
#if USE(DXDRM)
    if (equalIgnoringCase(keySystem, "com.microsoft.playready")
        || equalIgnoringCase(keySystem, "com.youtube.playready")) {
        if (!m_dxdrmSession)
            m_dxdrmSession = new DiscretixSession(this);
        unsigned short errorCode;
        unsigned long systemCode;
        RefPtr<Uint8Array> initData = Uint8Array::create(initDataPtr, initDataLength);
        String destinationURL;
        RefPtr<Uint8Array> result = m_dxdrmSession->dxdrmGenerateKeyRequest(initData.get(), destinationURL, errorCode, systemCode);
        if (errorCode)
            return MediaPlayer::InvalidPlayerState;

        URL url(URL(), destinationURL);
        m_player->keyMessage(keySystem, createCanonicalUUIDString(), result->data(), result->length(), url);
        return MediaPlayer::NoError;
    }
#endif

    if (!equalIgnoringCase(keySystem, "org.w3.clearkey"))
        return MediaPlayer::KeySystemNotSupported;

    m_player->keyMessage(keySystem, createCanonicalUUIDString(), initDataPtr, initDataLength, URL());
    return MediaPlayer::NoError;
}

MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerMSE::cancelKeyRequest(const String& /* keySystem */ , const String& /* sessionID */)
{
    LOG_MEDIA_MESSAGE("cancelKeyRequest");
    return MediaPlayer::KeySystemNotSupported;
}

void MediaPlayerPrivateGStreamerMSE::needKey(const String& keySystem, const String& sessionId, const unsigned char* initData, unsigned initDataLength)
{
    if (!m_player->keyNeeded(keySystem, sessionId, initData, initDataLength)) {
        GST_DEBUG("no event handler for key needed, waking up GStreamer thread");
        m_drmKeySemaphore.signal();
    }
}
#endif

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
void MediaPlayerPrivateGStreamerMSE::signalDRM()
{
    GST_DEBUG("key/license was changed or failed, signal semaphore");
    // Wake up a potential waiter blocked in the GStreamer thread
    m_drmKeySemaphore.signal();
}
#endif

#if ENABLE(ENCRYPTED_MEDIA_V2)
void MediaPlayerPrivateGStreamerMSE::needKey(RefPtr<Uint8Array> initData)
{
    if (!m_player->keyNeeded(initData.get())) {
        GST_DEBUG("no event handler for key needed, waking up GStreamer thread");
        m_drmKeySemaphore.signal();
    }
}

std::unique_ptr<CDMSession> MediaPlayerPrivateGStreamerMSE::createSession(const String& keySystem)
{
    if (!supportsKeySystem(keySystem, emptyString()))
        return nullptr;

#if USE(DXDRM)
    if (equalIgnoringCase(keySystem, "com.microsoft.playready")
        || equalIgnoringCase(keySystem, "com.youtube.playready"))
        return std::make_unique<CDMPRSessionGStreamer>(this);
#endif

    return nullptr;
}

void MediaPlayerPrivateGStreamerMSE::setCDMSession(CDMSession* session)
{
    m_cdmSession = session;
}

void MediaPlayerPrivateGStreamerMSE::keyAdded()
{
#if USE(DXDRM)
    CDMPRSessionGStreamer* session = static_cast<CDMPRSessionGStreamer*>(m_cdmSession);
    gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
       gst_structure_new("dxdrm-session", "session", G_TYPE_POINTER, static_cast<DiscretixSession*>(session), nullptr)));
#endif
}
#endif

void MediaPlayerPrivateGStreamerMSE::trackDetected(RefPtr<AppendPipeline> ap, RefPtr<WebCore::TrackPrivateBase> oldTrack, RefPtr<WebCore::TrackPrivateBase> newTrack)
{
    ASSERT (ap->track() == newTrack);

    printf("### %s: %s\n", __PRETTY_FUNCTION__, newTrack->id().string().latin1().data()); fflush(stdout);

    if (!oldTrack)
        m_playbackPipeline->attachTrack(ap->sourceBufferPrivate(), newTrack, ap->demuxersrcpadcaps());
    else
        m_playbackPipeline->reattachTrack(ap->sourceBufferPrivate(), newTrack, ap->demuxersrcpadcaps());
}

MediaPlayer::SupportsType MediaPlayerPrivateGStreamerMSE::supportsType(const MediaEngineSupportParameters& parameters)
{
    MediaPlayer::SupportsType result = MediaPlayer::IsNotSupported;
    if (!parameters.isMediaSource)
        return result;

    // Disable VPX/Opus on MSE for now, mp4/avc1 seems way more reliable currently.
    if (parameters.type.endsWith("webm"))
        return result;

    // Youtube TV provides empty types for some videos and we want to be selected as best media engine for them.
    if (parameters.type.isNull() || parameters.type.isEmpty()) {
        result = MediaPlayer::MayBeSupported;
        return result;
    }

    // spec says we should not return "probably" if the codecs string is empty
    if (mimeTypeCache().contains(parameters.type))
        result = parameters.codecs.isEmpty() ? MediaPlayer::MayBeSupported : MediaPlayer::IsSupported;

#if ENABLE(ENCRYPTED_MEDIA)
    // From: <http://dvcs.w3.org/hg/html-media/raw-file/eme-v0.1b/encrypted-media/encrypted-media.html#dom-canplaytype>
    // In addition to the steps in the current specification, this method must run the following steps:

    // 1. Check whether the Key System is supported with the specified container and codec type(s) by following the steps for the first matching condition from the following list:
    //    If keySystem is null, continue to the next step.
    if (parameters.keySystem.isNull() || parameters.keySystem.isEmpty())
        return result;

    // If keySystem contains an unrecognized or unsupported Key System, return the empty string
    if (supportsKeySystem(parameters.keySystem, emptyString()))
        result = MediaPlayer::IsSupported;
#endif

    return result;
}

void MediaPlayerPrivateGStreamerMSE::setDownloadBuffering()
{
    if (!m_pipeline)
        return;

    if (isMediaSource())
        return;

    unsigned flags;
    g_object_get(m_pipeline.get(), "flags", &flags, nullptr);

    unsigned flagDownload = getGstPlayFlag("download");

    // We don't want to stop downloading if we already started it.
    if (flags & flagDownload && m_readyState > MediaPlayer::HaveNothing && !m_resetPipeline)
        return;

    bool shouldDownload = !isLiveStream() && m_preload == MediaPlayer::Auto;
    if (shouldDownload) {
        LOG_MEDIA_MESSAGE("Enabling on-disk buffering");
        g_object_set(m_pipeline.get(), "flags", flags | flagDownload, nullptr);
        m_fillTimer.startRepeating(0.2);
    } else {
        LOG_MEDIA_MESSAGE("Disabling on-disk buffering");
        g_object_set(m_pipeline.get(), "flags", flags & ~flagDownload, nullptr);
        m_fillTimer.stop();
    }
}

void MediaPlayerPrivateGStreamerMSE::setPreload(MediaPlayer::Preload preload)
{
    if (preload == MediaPlayer::Auto && isLiveStream())
        return;

    m_preload = preload;
    setDownloadBuffering();

    if (m_delayingLoad && m_preload != MediaPlayer::None) {
        m_delayingLoad = false;
        commitLoad();
    }
}

GstElement* MediaPlayerPrivateGStreamerMSE::createAudioSink()
{
    m_autoAudioSink = gst_element_factory_make("autoaudiosink", 0);
    g_signal_connect(m_autoAudioSink.get(), "child-added", G_CALLBACK(setAudioStreamPropertiesCallback), this);

    GstElement* audioSinkBin;

    if (webkitGstCheckVersion(1, 4, 2)) {
#if ENABLE(WEB_AUDIO)
        audioSinkBin = gst_bin_new("audio-sink");
        m_audioSourceProvider->configureAudioBin(audioSinkBin, nullptr);
        return audioSinkBin;
#else
        return m_autoAudioSink.get();
#endif
    }

    // Construct audio sink only if pitch preserving is enabled.
    // If GStreamer 1.4.2 is used the audio-filter playbin property is used instead.
    if (m_preservesPitch) {
        GstElement* scale = gst_element_factory_make("scaletempo", nullptr);
        if (!scale) {
            GST_WARNING("Failed to create scaletempo");
            return m_autoAudioSink.get();
        }

        audioSinkBin = gst_bin_new("audio-sink");
        gst_bin_add(GST_BIN(audioSinkBin), scale);
        GRefPtr<GstPad> pad = adoptGRef(gst_element_get_static_pad(scale, "sink"));
        gst_element_add_pad(audioSinkBin, gst_ghost_pad_new("sink", pad.get()));

#if ENABLE(WEB_AUDIO)
        m_audioSourceProvider->configureAudioBin(audioSinkBin, scale);
#else
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* resample = gst_element_factory_make("audioresample", nullptr);

        gst_bin_add_many(GST_BIN(audioSinkBin), convert, resample, m_autoAudioSink.get(), nullptr);

        if (!gst_element_link_many(scale, convert, resample, m_autoAudioSink.get(), nullptr)) {
            GST_WARNING("Failed to link audio sink elements");
            gst_object_unref(audioSinkBin);
            return m_autoAudioSink.get();
        }
#endif
        return audioSinkBin;
    }

#if ENABLE(WEB_AUDIO)
    audioSinkBin = gst_bin_new("audio-sink");
    m_audioSourceProvider->configureAudioBin(audioSinkBin, nullptr);
    return audioSinkBin;
#endif
    ASSERT_NOT_REACHED();
    return 0;
}

GstElement* MediaPlayerPrivateGStreamerMSE::audioSink() const
{
    GstElement* sink;
    g_object_get(m_pipeline.get(), "audio-sink", &sink, nullptr);
    return sink;

    GRefPtr<GstElement> playsink = adoptGRef(gst_bin_get_by_name(GST_BIN(m_pipeline.get()), "playsink"));
    if (playsink) {
        // The default value (0) means "send events to all the sinks", instead
        // of "only to the first that returns true". This is needed for MSE seek.
        g_object_set(G_OBJECT(playsink.get()), "send-event-mode", 0, NULL);
    }
}

void MediaPlayerPrivateGStreamerMSE::configurePlaySink()
{
    GRefPtr<GstElement> playsink = adoptGRef(gst_bin_get_by_name(GST_BIN(m_pipeline.get()), "playsink"));
    if (playsink) {
        // The default value (0) means "send events to all the sinks", instead
        // of "only to the first that returns true". This is needed for MSE seek.
        g_object_set(G_OBJECT(playsink.get()), "send-event-mode", 0, NULL);
    }
}

void MediaPlayerPrivateGStreamerMSE::createGSTPlayBin()
{
    ASSERT(!m_pipeline);

    // gst_element_factory_make() returns a floating reference so
    // we should not adopt.
    setPipeline(gst_element_factory_make("playbin", "play"));
    setStreamVolumeElement(GST_STREAM_VOLUME(m_pipeline.get()));

    unsigned flagText = getGstPlayFlag("text");
    unsigned flagAudio = getGstPlayFlag("audio");
    unsigned flagVideo = getGstPlayFlag("video");
    unsigned flagNativeVideo = getGstPlayFlag("native-video");
    g_object_set(m_pipeline.get(), "flags", flagText | flagAudio | flagVideo | flagNativeVideo, nullptr);

    GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
    gst_bus_add_signal_watch(bus.get());
    g_signal_connect(bus.get(), "message", G_CALLBACK(mediaPlayerPrivateMessageCallback), this);

    gst_bus_enable_sync_message_emission(bus.get());
    g_signal_connect(bus.get(), "sync-message", G_CALLBACK(mediaPlayerPrivateSyncMessageCallback), this);

    g_object_set(m_pipeline.get(), "mute", m_player->muted(), nullptr);

    g_signal_connect(m_pipeline.get(), "notify::source", G_CALLBACK(mediaPlayerPrivateSourceChangedCallback), this);

    // If we load a MediaSource later, we will also listen the signals from
    // WebKitMediaSrc, which will be connected later in sourceChanged()
    // METRO FIXME: In that case, we shouldn't listen to these signals coming from playbin, or the callbacks will be called twice.
    g_signal_connect(m_pipeline.get(), "video-changed", G_CALLBACK(mediaPlayerPrivateVideoChangedCallback), this);
    g_signal_connect(m_pipeline.get(), "audio-changed", G_CALLBACK(mediaPlayerPrivateAudioChangedCallback), this);
    if (webkitGstCheckVersion(1, 1, 2)) {
        g_signal_connect(m_pipeline.get(), "text-changed", G_CALLBACK(mediaPlayerPrivateTextChangedCallback), this);

        GstElement* textCombiner = webkitTextCombinerNew();
        ASSERT(textCombiner);
        g_object_set(m_pipeline.get(), "text-stream-combiner", textCombiner, nullptr);

        m_textAppSink = webkitTextSinkNew();
        ASSERT(m_textAppSink);

        m_textAppSinkPad = adoptGRef(gst_element_get_static_pad(m_textAppSink.get(), "sink"));
        ASSERT(m_textAppSinkPad);

        g_object_set(m_textAppSink.get(), "emit-signals", true, "enable-last-sample", false, "caps", gst_caps_new_empty_simple("text/vtt"), NULL);
        g_signal_connect(m_textAppSink.get(), "new-sample", G_CALLBACK(mediaPlayerPrivateNewTextSampleCallback), this);

        g_object_set(m_pipeline.get(), "text-sink", m_textAppSink.get(), NULL);
    }

    g_object_set(m_pipeline.get(), "video-sink", createVideoSink(), "audio-sink", createAudioSink(), nullptr);
    configurePlaySink();

    // On 1.4.2 and newer we use the audio-filter property instead.
    // See https://bugzilla.gnome.org/show_bug.cgi?id=735748 for
    // the reason for using >= 1.4.2 instead of >= 1.4.0.
    if (m_preservesPitch && webkitGstCheckVersion(1, 4, 2)) {
        GstElement* scale = gst_element_factory_make("scaletempo", 0);

        if (!scale)
            GST_WARNING("Failed to create scaletempo");
        else
            g_object_set(m_pipeline.get(), "audio-filter", scale, nullptr);
    }

    GRefPtr<GstPad> videoSinkPad = adoptGRef(gst_element_get_static_pad(m_videoSink.get(), "sink"));
    if (videoSinkPad)
        g_signal_connect(videoSinkPad.get(), "notify::caps", G_CALLBACK(mediaPlayerPrivateVideoSinkCapsChangedCallback), this);
}

void MediaPlayerPrivateGStreamerMSE::simulateAudioInterruption()
{
    GstMessage* message = gst_message_new_request_state(GST_OBJECT(m_pipeline.get()), GST_STATE_PAUSED);
    gst_element_post_message(m_pipeline.get(), message);
}

bool MediaPlayerPrivateGStreamerMSE::didPassCORSAccessCheck() const
{
    if (WEBKIT_IS_WEB_SRC(m_source.get()))
        return webKitSrcPassedCORSAccessCheck(WEBKIT_WEB_SRC(m_source.get()));
    return false;
}

bool MediaPlayerPrivateGStreamerMSE::canSaveMediaData() const
{
    if (isLiveStream())
        return false;

    if (m_url.isLocalFile())
        return true;

    if (m_url.protocolIsInHTTPFamily())
        return true;

    return false;
}


class GStreamerMediaDescription : public MediaDescription {
private:
    GstCaps* m_caps;
public:
    static PassRefPtr<GStreamerMediaDescription> create(GstCaps* caps)
    {
        return adoptRef(new GStreamerMediaDescription(caps));
    }

    virtual ~GStreamerMediaDescription()
    {
        gst_caps_unref(m_caps);
    }

    AtomicString codec() const override
    {
        gchar* description = gst_pb_utils_get_codec_description(m_caps);
        AtomicString codecName(description);
        g_free(description);

        return codecName;
    }

    bool isVideo() const override
    {
        GstStructure* s = gst_caps_get_structure(m_caps, 0);
        const gchar* name = gst_structure_get_name(s);

#if GST_CHECK_VERSION(1, 5, 3)
        if (!g_strcmp0(name, "application/x-cenc"))
            return g_str_has_prefix(gst_structure_get_string(s, "original-media-type"), "video/");
#endif
        return g_str_has_prefix(name, "video/");
    }

    bool isAudio() const override
    {
        GstStructure* s = gst_caps_get_structure(m_caps, 0);
        const gchar* name = gst_structure_get_name(s);

#if GST_CHECK_VERSION(1, 5, 3)
        if (!g_strcmp0(name, "application/x-cenc"))
            return g_str_has_prefix(gst_structure_get_string(s, "original-media-type"), "audio/");
#endif
        return g_str_has_prefix(name, "audio/");
    }

    bool isText() const override
    {
        // TODO
        return false;
    }

private:
    GStreamerMediaDescription(GstCaps* caps)
        : MediaDescription()
        , m_caps(gst_caps_ref(caps))
    {
    }
};

// class GStreamerMediaSample : public MediaSample
GStreamerMediaSample::GStreamerMediaSample(GstSample* sample, const FloatSize& presentationSize, const AtomicString& trackID)
    : MediaSample()
    , m_pts(MediaTime::zeroTime())
    , m_dts(MediaTime::zeroTime())
    , m_duration(MediaTime::zeroTime())
    , m_trackID(trackID)
    , m_size(0)
    , m_sample(0)
    , m_presentationSize(presentationSize)
    , m_flags(MediaSample::IsSync)
{

    if (!sample)
        return;

    GstBuffer* buffer = gst_sample_get_buffer(sample);

    if (!buffer)
        return;

    if (GST_BUFFER_PTS_IS_VALID(buffer))
        m_pts = MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND);
    if (GST_BUFFER_DTS_IS_VALID(buffer))
        m_dts = MediaTime(GST_BUFFER_DTS(buffer), GST_SECOND);
    if (GST_BUFFER_DURATION_IS_VALID(buffer))
        m_duration = MediaTime(GST_BUFFER_DURATION(buffer), GST_SECOND);
    m_size = gst_buffer_get_size(buffer);
    m_sample = gst_sample_ref(sample);

    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
        m_flags = MediaSample::None;

    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY))
        m_flags = (MediaSample::SampleFlags) (m_flags | MediaSample::NonDisplaying);
}

PassRefPtr<GStreamerMediaSample> GStreamerMediaSample::create(GstSample* sample, const FloatSize& presentationSize, const AtomicString& trackID)
{
    return adoptRef(new GStreamerMediaSample(sample, presentationSize, trackID));
}

PassRefPtr<GStreamerMediaSample> GStreamerMediaSample::createFakeSample(GstCaps* caps, MediaTime pts, MediaTime dts, MediaTime duration, const FloatSize& presentationSize, const AtomicString& trackID)
{
    GstSample* sample = gst_sample_new(0, gst_caps_ref(caps), 0, 0);
    GStreamerMediaSample* s = new GStreamerMediaSample(sample, presentationSize, trackID);
    s->m_pts = pts;
    s->m_dts = dts;
    s->m_duration = duration;
    s->m_flags = MediaSample::NonDisplaying;
    return adoptRef(s);
}

GStreamerMediaSample::~GStreamerMediaSample()
{
    if (m_sample)
        gst_sample_unref(m_sample);
}


// Auxiliar to pass several parameters to appendPipelineDemuxerPadXXXMainThread().
class DemuxerPadInfo
{
public:
    DemuxerPadInfo(GstPad* demuxerSrcPad, AppendPipeline* appendPipeline)
        : m_pad(demuxerSrcPad)
        , m_ap(appendPipeline)
    {
        gst_object_ref(m_pad);
    }
    virtual ~DemuxerPadInfo()
    {
        gst_object_unref(m_pad);
    }

    GstPad* pad() { return m_pad; }
    RefPtr<AppendPipeline> ap() { return m_ap; }

private:
    GstPad* m_pad;
    RefPtr<AppendPipeline> m_ap;
};

// Auxiliar to pass several parameters to appendPipelineAppSinkNewSampleMainThread().
class NewSampleInfo
{
public:
    NewSampleInfo(GstSample* sample, AppendPipeline* appendPipeline)
        : m_sample(sample)
        , m_ap(appendPipeline)
    {
        gst_sample_ref(m_sample);
    }
    virtual ~NewSampleInfo()
    {
        gst_sample_unref(m_sample);
    }

    GstSample* sample() { return m_sample; }
    RefPtr<AppendPipeline> ap() { return m_ap; }

private:
    GstSample* m_sample;
    RefPtr<AppendPipeline> m_ap;
};

static const char* dumpAppendStage(AppendPipeline::AppendStage appendStage)
{
    enum AppendStage { Invalid, NotStarted, Ongoing, NoDataToDecode, Sampling, LastSample, Aborting };

    switch (appendStage) {
    case AppendPipeline::AppendStage::Invalid:
        return "Invalid";
    case AppendPipeline::AppendStage::NotStarted:
        return "NotStarted";
    case AppendPipeline::AppendStage::Ongoing:
        return "Ongoing";
    case AppendPipeline::AppendStage::NoDataToDecode:
        return "NoDataToDecode";
    case AppendPipeline::AppendStage::Sampling:
        return "Sampling";
    case AppendPipeline::AppendStage::LastSample:
        return "LastSample";
    case AppendPipeline::AppendStage::Aborting:
        return "Aborting";
    default:
        return "?";
    }
}

static void appendPipelineDemuxerPadAdded(GstElement*, GstPad*, AppendPipeline*);
static gboolean appendPipelineDemuxerPadAddedMainThread(DemuxerPadInfo*);
static void appendPipelineDemuxerPadRemoved(GstElement*, GstPad*, AppendPipeline*);
static gboolean appendPipelineDemuxerPadRemovedMainThread(DemuxerPadInfo*);
static void appendPipelineAppSinkCapsChanged(GObject*, GParamSpec*, AppendPipeline*);
static gboolean appendPipelineAppSinkCapsChangedMainThread(AppendPipeline*);
static void appendPipelineAppSinkNewSample(GstElement*, AppendPipeline*);
static gboolean appendPipelineAppSinkNewSampleMainThread(NewSampleInfo*);
static void appendPipelineAppSinkEOS(GstElement*, AppendPipeline*);
static gboolean appendPipelineAppSinkEOSMainThread(AppendPipeline* ap);
static gboolean appendPipelineNoDataToDecodeTimeout(AppendPipeline* ap);
static gboolean appendPipelineLastSampleTimeout(AppendPipeline* ap);

AppendPipeline::AppendPipeline(PassRefPtr<MediaSourceClientGStreamerMSE> mediaSourceClient, PassRefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, MediaPlayerPrivateGStreamerMSE* playerPrivate)
    : m_mediaSourceClient(mediaSourceClient)
    , m_sourceBufferPrivate(sourceBufferPrivate)
    , m_playerPrivate(playerPrivate)
    , m_id(0)
    , m_appsink(NULL)
    , m_demuxersrcpadcaps(NULL)
    , m_noDataToDecodeTimeoutTag(0)
    , m_lastSampleTimeoutTag(0)
    , m_appendStage(NotStarted)
    , m_abortPending(false)
    , m_streamType(Unknown)
{
    printf("### %s %p\n", __PRETTY_FUNCTION__, this); fflush(stdout);

    m_pipeline = gst_pipeline_new(NULL);

    m_appsrc = gst_element_factory_make("appsrc", NULL);
    m_typefind = gst_element_factory_make("typefind", NULL);
    m_qtdemux = gst_element_factory_make("qtdemux", NULL);
    m_appsink = gst_element_factory_make("appsink", NULL);
    gst_app_sink_set_emit_signals(GST_APP_SINK(m_appsink), TRUE);
    gst_base_sink_set_sync(GST_BASE_SINK(m_appsink), FALSE);

    // These signals won't be connected outside of the lifetime of "this".
    g_signal_connect(m_qtdemux, "pad-added", G_CALLBACK(appendPipelineDemuxerPadAdded), this);
    g_signal_connect(m_qtdemux, "pad-removed", G_CALLBACK(appendPipelineDemuxerPadRemoved), this);
    g_signal_connect(m_appsink, "new-sample", G_CALLBACK(appendPipelineAppSinkNewSample), this);
    g_signal_connect(m_appsink, "eos", G_CALLBACK(appendPipelineAppSinkEOS), this);

    GstPad* appsinkpad = gst_element_get_static_pad(m_appsink, "sink");
    g_signal_connect(appsinkpad, "notify::caps", G_CALLBACK(appendPipelineAppSinkCapsChanged), this);

    // Add_many will take ownership of a reference. Request one ref more for ourselves.
    gst_object_ref(m_appsrc);
    gst_object_ref(m_typefind);
    gst_object_ref(m_qtdemux);
    gst_object_ref(m_appsink);

    gst_bin_add_many(GST_BIN(m_pipeline), m_appsrc, m_typefind, m_qtdemux, m_appsink, NULL);
    gst_element_link_many(m_appsrc, m_typefind, m_qtdemux, NULL);

    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);

    gst_object_unref(appsinkpad);
};

AppendPipeline::~AppendPipeline()
{
    printf("### %s %p\n", __PRETTY_FUNCTION__, this); fflush(stdout);
    if (m_noDataToDecodeTimeoutTag) {
        printf("### %s: m_noDataToDecodeTimeoutTag=%u\n", __PRETTY_FUNCTION__, m_noDataToDecodeTimeoutTag); fflush(stdout);
        // TODO: Maybe notify appendComplete here?
        g_source_remove(m_noDataToDecodeTimeoutTag);
        m_noDataToDecodeTimeoutTag = 0;
    }

    if (m_lastSampleTimeoutTag) {
        printf("### %s: m_lastSampleTimeoutTag=%u\n", __PRETTY_FUNCTION__, m_lastSampleTimeoutTag); fflush(stdout);
        // TODO: Maybe notify appendComplete here?
        g_source_remove(m_lastSampleTimeoutTag);
        m_lastSampleTimeoutTag = 0;
    }

    if (m_pipeline) {
        gst_element_set_state (m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = NULL;
    }

    if (m_appsrc) {
        GstPad* appsinkpad = gst_element_get_static_pad(m_appsink, "sink");
        g_signal_handlers_disconnect_by_func(appsinkpad, reinterpret_cast<gpointer>(appendPipelineAppSinkCapsChanged), this);
        gst_object_unref(appsinkpad);
        gst_object_unref(m_appsrc);
        m_appsrc = NULL;
    }

    if (m_typefind) {
        gst_object_unref(m_typefind);
        m_typefind = NULL;
    }

    if (m_qtdemux) {
        g_signal_handlers_disconnect_by_func(m_qtdemux, (gpointer)appendPipelineDemuxerPadAdded, this);
        g_signal_handlers_disconnect_by_func(m_qtdemux, (gpointer)appendPipelineDemuxerPadRemoved, this);

        gst_object_unref(m_qtdemux);
        m_qtdemux = NULL;
    }

    if (m_appsink) {
        g_signal_handlers_disconnect_by_func(m_appsink, (gpointer)appendPipelineAppSinkNewSample, this);
        g_signal_handlers_disconnect_by_func(m_appsink, (gpointer)appendPipelineAppSinkEOS, this);

        gst_object_unref(m_appsink);
        m_appsink = NULL;
    }

    if (m_demuxersrcpadcaps) {
        gst_caps_unref(m_demuxersrcpadcaps);
        m_demuxersrcpadcaps = NULL;
    }
};

gint AppendPipeline::id()
{
    static gint totalAudio = 0;
    static gint totalVideo = 0;
    static gint totalText = 0;

    if (m_id)
        return m_id;

    switch (m_streamType) {
    case Audio:
        totalAudio++;
        m_id = totalAudio;
        break;
    case Video:
        totalVideo++;
        m_id = totalVideo;
        break;
    case Text:
        totalText++;
        m_id = totalText;
        break;
    case Unknown:
        printf("### %s: Trying to get id for a pipeline of Unknown type\n", __PRETTY_FUNCTION__); fflush(stdout);
        g_assert_not_reached();
        break;
    }

    printf("### %s: streamType=%d, id=%d\n", __PRETTY_FUNCTION__, static_cast<int>(m_streamType), m_id); fflush(stdout);

    return m_id;
}

void AppendPipeline::setAppendStage(AppendStage newAppendStage)
{
    // Valid transitions:
    // NotStarted-->Ongoing-->NoDataToDecode-->NotStarted
    //           |         |                `->Aborting-->NotStarted
    //           |         `->Sampling-···->Sampling-->LastSample-->NotStarted
    //           |                                               `->Aborting-->NotStarted
    //           `->Aborting-->NotStarted
    AppendStage oldAppendStage = m_appendStage;
    AppendStage nextAppendStage = Invalid;

    bool ok = false;

    printf("### %s: %s --> %s\n", __PRETTY_FUNCTION__, dumpAppendStage(oldAppendStage), dumpAppendStage(newAppendStage)); fflush(stdout);

    switch (oldAppendStage) {
    case NotStarted:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        switch (newAppendStage) {
        case Ongoing:
            ok = true;
            m_noDataToDecodeTimeoutTag = g_timeout_add(s_noDataToDecodeTimeoutMsec, GSourceFunc(appendPipelineNoDataToDecodeTimeout), this);
            break;
        case NotStarted:
            ok = true;
            break;
        case Aborting:
            ok = true;
            nextAppendStage = NotStarted;
            break;
        default:
            break;
        }
        break;
    case Ongoing:
        g_assert(m_noDataToDecodeTimeoutTag != 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        switch (newAppendStage) {
        case NoDataToDecode:
            ok = true;
            if (m_noDataToDecodeTimeoutTag) {
                g_source_remove(m_noDataToDecodeTimeoutTag);
                m_noDataToDecodeTimeoutTag = 0;
            }
            m_mediaSourceClient->didReceiveAllPendingSamples(m_sourceBufferPrivate.get());
            if (m_abortPending)
                nextAppendStage = Aborting;
            else
                nextAppendStage = NotStarted;
            break;
        case Sampling:
            ok = true;
            if (m_noDataToDecodeTimeoutTag) {
                g_source_remove(m_noDataToDecodeTimeoutTag);
                m_noDataToDecodeTimeoutTag = 0;
            }

            if (m_lastSampleTimeoutTag) {
                printf("### %s: lastSampleTimeoutTag already exists while transitioning Ongoing-->Sampling\n", __PRETTY_FUNCTION__); fflush(stdout);
                g_source_remove(m_lastSampleTimeoutTag);
                m_lastSampleTimeoutTag = 0;
            }
            m_lastSampleTimeoutTag = g_timeout_add(s_lastSampleTimeoutMsec, GSourceFunc(appendPipelineLastSampleTimeout), this);
            break;
        default:
            break;
        }
        break;
    case NoDataToDecode:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        switch (newAppendStage) {
        case NotStarted:
            ok = true;
            break;
        case Aborting:
            ok = true;
            nextAppendStage = NotStarted;
            break;
        default:
            break;
        }
        break;
    case Sampling:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag != 0);
        switch (newAppendStage) {
        case Sampling:
            ok = true;
            if (m_lastSampleTimeoutTag) {
                g_source_remove(m_lastSampleTimeoutTag);
                m_lastSampleTimeoutTag = 0;
            }
            m_lastSampleTimeoutTag = g_timeout_add(s_lastSampleTimeoutMsec, GSourceFunc(appendPipelineLastSampleTimeout), this);
            break;
        case LastSample:
            ok = true;
            if (m_lastSampleTimeoutTag) {
                g_source_remove(m_lastSampleTimeoutTag);
                m_lastSampleTimeoutTag = 0;
            }
            m_mediaSourceClient->didReceiveAllPendingSamples(m_sourceBufferPrivate.get());
            if (m_abortPending)
                nextAppendStage = Aborting;
            else
                nextAppendStage = NotStarted;
            break;
        default:
            break;
        }
        break;
    case LastSample:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        switch (newAppendStage) {
        case NotStarted:
            ok = true;
            break;
        case Aborting:
            ok = true;
            nextAppendStage = NotStarted;
            break;
        default:
            break;
        }
        break;
    case Aborting:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        switch (newAppendStage) {
        case NotStarted:
            ok = true;
            resetPipeline();
            m_abortPending = false;
            break;
        default:
            break;
        }
        break;
    case Invalid:
        g_assert(m_noDataToDecodeTimeoutTag == 0);
        g_assert(m_lastSampleTimeoutTag == 0);
        break;
    }

    if (ok)
        m_appendStage = newAppendStage;
    else {
        printf("### %s: Invalid append stage transition %s --> %s\n", __PRETTY_FUNCTION__, dumpAppendStage(oldAppendStage), dumpAppendStage(newAppendStage)); fflush(stdout);
    }

    g_assert(ok);

    if (nextAppendStage != Invalid)
        setAppendStage(nextAppendStage);
}

// Takes ownership of caps.
void AppendPipeline::updatePresentationSize(GstCaps* demuxersrcpadcaps)
{
    if (m_demuxersrcpadcaps)
        gst_caps_unref(m_demuxersrcpadcaps);
    m_demuxersrcpadcaps = demuxersrcpadcaps;

    GstStructure* s = gst_caps_get_structure(m_demuxersrcpadcaps, 0);
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

            m_presentationSize = WebCore::FloatSize(width, finalHeight);
        } else
            m_presentationSize = WebCore::FloatSize();
        sizeConfigured = true;
    }
#endif

    if (!sizeConfigured) {
        if (g_str_has_prefix(structureName, "video/") && gst_video_info_from_caps(&info, demuxersrcpadcaps)) {
            float width, height;

            width = info.width;
            height = info.height * ((float) info.par_d / (float) info.par_n);

            m_presentationSize = WebCore::FloatSize(width, height);
        } else
            m_presentationSize = WebCore::FloatSize();
    }
}

void AppendPipeline::demuxerPadAdded(GstPad* demuxersrcpad)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);
    // TODO: Update presentation size, see webKitMediaSrcUpdatePresentationSize().

    /*
    updatePresentationSize(gst_pad_get_current_caps(demuxersrcpad));

    RefPtr<WebCore::TrackPrivateBase> oldTrack = m_track;
    GstStructure* s = gst_caps_get_structure(m_demuxersrcpadcaps, 0);
    const gchar* mediaType = gst_structure_get_name(s);

    if (g_str_has_prefix(mediaType, "audio")) {
        m_streamType = Audio;
        m_track = WebCore::AudioTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), demuxersrcpad);
    } else if (g_str_has_prefix(mediaType, "video")) {
        m_streamType = Video;
        m_track = WebCore::VideoTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), demuxersrcpad);
    } else if (g_str_has_prefix(mediaType, "text")) {
        m_streamType = Text;
        m_track = WebCore::InbandTextTrackPrivateGStreamer::create(id(), demuxersrcpad);
    } else {
        // No useful data, but notify anyway to complete the append operation (webKitMediaSrcLastSampleTimeout is cancelled and won't notify in this case)
        // TODO: EME uses its own special types.
        printf("### %s: (no data)\n", __PRETTY_FUNCTION__); fflush(stdout);
        m_mediaSourceClient->didReceiveAllPendingSamples(m_sourceBufferPrivate.get());
        return;
    }

    m_playerPrivate->trackDetected(this, oldTrack, m_track);
    didReceiveInitializationSegment();
    */
}

void AppendPipeline::demuxerPadRemoved(GstPad*)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);
    // TODO: Remove this method if it's useless in the end.
}

void AppendPipeline::appSinkCapsChanged()
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    GstPad* appsinkpad = gst_element_get_static_pad(m_appsink, "sink");
    GstCaps* caps = gst_pad_get_current_caps(GST_PAD(appsinkpad));

    {
        gchar* strcaps = gst_caps_to_string(caps);
        printf("!!! %s: %s\n", __PRETTY_FUNCTION__, strcaps); fflush(stdout);
        g_free(strcaps);
    }

    if (!caps) {
        gst_object_unref(appsinkpad);
        return;
    }

    updatePresentationSize(gst_caps_ref(caps));

    RefPtr<WebCore::TrackPrivateBase> oldTrack = m_track;
    GstStructure* s = gst_caps_get_structure(m_demuxersrcpadcaps, 0);
    const gchar* mediaType = gst_structure_get_name(s);
    bool isData;

    if (g_str_has_prefix(mediaType, "audio")) {
        isData = true;
        m_streamType = Audio;
        m_track = WebCore::AudioTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), appsinkpad);
    } else if (g_str_has_prefix(mediaType, "video")) {
        isData = true;
        m_streamType = Video;
        m_track = WebCore::VideoTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), appsinkpad);
    } else if (g_str_has_prefix(mediaType, "text")) {
        isData = true;
        m_streamType = Text;
        m_track = WebCore::InbandTextTrackPrivateGStreamer::create(id(), appsinkpad);
    } else {
        // No useful data, but notify anyway to complete the append operation (webKitMediaSrcLastSampleTimeout is cancelled and won't notify in this case)
        // TODO: EME uses its own special types.
        printf("### %s: (no data)\n", __PRETTY_FUNCTION__); fflush(stdout);
        isData = false;
        m_mediaSourceClient->didReceiveAllPendingSamples(m_sourceBufferPrivate.get());
    }

    if (isData) {
        m_playerPrivate->trackDetected(this, oldTrack, m_track);
        didReceiveInitializationSegment();
    }

    gst_caps_unref(caps);
    gst_object_unref(appsinkpad);
}

void AppendPipeline::appSinkNewSample(GstSample* sample)
{
    // Ignore samples if we're not expecting them.
    if (!(m_appendStage == Ongoing || m_appendStage == Sampling)) {
        printf("### %s: Unexpected sample\n", __PRETTY_FUNCTION__); fflush(stdout);
        return;
    }

    AtomicString trackId(AppendPipeline::trackId());

    RefPtr<GStreamerMediaSample> mediaSample = WebCore::GStreamerMediaSample::create(sample, m_presentationSize, trackId);

    printf("### %s: trackId=%s PTS=%f presentationSize=%.0fx%.0f\n", __PRETTY_FUNCTION__, trackId.string().utf8().data(), mediaSample->presentationTime().toFloat(), mediaSample->presentationSize().width(), mediaSample->presentationSize().height()); fflush(stdout);

    // If we're beyond the duration, ignore this sample and the remaining ones.
    MediaTime duration = m_mediaSourceClient->duration();
    if (duration.isValid() && !duration.indefiniteTime() && mediaSample->presentationTime() > duration) {
        printf("### %s: Detected sample (%f) beyond the duration (%f), declaring LastSample\n", __PRETTY_FUNCTION__, mediaSample->presentationTime().toFloat(), duration.toFloat()); fflush(stdout);
        setAppendStage(LastSample);
        return;
    }

    MediaTime timestampOffset(MediaTime::createWithDouble(m_sourceBufferPrivate->timestampOffset()));

    // Add a fake sample if a gap is detected before the first sample
    if (mediaSample->presentationTime() >= timestampOffset &&
        mediaSample->presentationTime() <= timestampOffset + MediaTime::createWithDouble(0.1)) {
        RefPtr<WebCore::GStreamerMediaSample> fakeSample = WebCore::GStreamerMediaSample::createFakeSample(
                gst_sample_get_caps(sample), timestampOffset, mediaSample->decodeTime(), mediaSample->presentationTime() - timestampOffset, mediaSample->presentationSize(),
                trackId);
        m_mediaSourceClient->didReceiveSample(m_sourceBufferPrivate.get(), fakeSample);
    }

    m_mediaSourceClient->didReceiveSample(m_sourceBufferPrivate.get(), mediaSample);
    setAppendStage(Sampling);
}

void AppendPipeline::appSinkEOS()
{
    switch (m_appendStage) {
    // Ignored. Operation completion will be managed by the Aborting->NotStarted transition.
    case Aborting:
        return;
    // Finish Ongoing and Sampling stages.
    case Ongoing:
        setAppendStage(NoDataToDecode);
        break;
    case Sampling:
        setAppendStage(LastSample);
        break;
    default:
        printf("### %s: Unexpected EOS\n", __PRETTY_FUNCTION__); fflush(stdout);
        break;
    }
}

void AppendPipeline::didReceiveInitializationSegment()
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    WebCore::SourceBufferPrivateClient::InitializationSegment initializationSegment;

    initializationSegment.duration = m_mediaSourceClient->duration();
    switch (m_streamType) {
    case Audio:
        {
            WebCore::SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation info;
            info.track = static_cast<AudioTrackPrivateGStreamer*>(m_track.get());
            info.description = WebCore::GStreamerMediaDescription::create(m_demuxersrcpadcaps);
            initializationSegment.audioTracks.append(info);
        }
        break;
    case Video:
        {
            WebCore::SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation info;
            info.track = static_cast<VideoTrackPrivateGStreamer*>(m_track.get());
            info.description = WebCore::GStreamerMediaDescription::create(m_demuxersrcpadcaps);
            initializationSegment.videoTracks.append(info);
        }
        break;
    default:
        printf("%s: Unsupported or unknown stream type\n", __PRETTY_FUNCTION__); fflush(stdout);
        ASSERT_NOT_REACHED();
        break;
    }

    m_mediaSourceClient->didReceiveInitializationSegment(m_sourceBufferPrivate.get(), initializationSegment);
}

AtomicString AppendPipeline::trackId()
{
    if (!m_track)
        return AtomicString();

    return m_track->id();
}

void AppendPipeline::resetPipeline()
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);
    gst_element_set_state(m_pipeline, GST_STATE_READY);
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);

    {
        static int i = 0;
        WTF::String  dotFileName = String::format("reset-pipeline-%d", ++i);
        gst_debug_bin_to_dot_file(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.utf8().data());
    }

}

void AppendPipeline::abort()
{
    // Abort already ongoing.
    if (m_abortPending)
        return;

    m_abortPending = true;
    if (m_appendStage == NotStarted)
        setAppendStage(Aborting);
    // Else, the automatic stage transitions will take care when the ongoing append finishes.
}

static void appendPipelineDemuxerPadAdded(GstElement*, GstPad* demuxersrcpad, AppendPipeline* ap)
{
    // Must be done in the streaming thread.
    GstPad* sinkpad = gst_element_get_static_pad(ap->appsink(), "sink");
    // Only one Stream per demuxer is supported.
    ASSERT(!gst_pad_is_linked(sinkpad));
    gst_pad_link(demuxersrcpad, sinkpad);
    gst_object_unref(sinkpad);

    if (WTF::isMainThread())
        ap->demuxerPadAdded(demuxersrcpad);
    else
        g_timeout_add(0, GSourceFunc(appendPipelineDemuxerPadAddedMainThread), new DemuxerPadInfo(demuxersrcpad, ap));
}

static gboolean appendPipelineDemuxerPadAddedMainThread(DemuxerPadInfo* info)
{
    if (info->ap()->qtdemux())
        info->ap()->demuxerPadAdded(info->pad());
    delete info;
    return G_SOURCE_REMOVE;
}

static void appendPipelineDemuxerPadRemoved(GstElement*, GstPad* demuxersrcpad, AppendPipeline* ap)
{
    // Must be done in the streaming thread.
    gst_element_unlink(ap->qtdemux(), ap->appsink());

    if (WTF::isMainThread())
        ap->demuxerPadRemoved(demuxersrcpad);
    else
        g_timeout_add(0, GSourceFunc(appendPipelineDemuxerPadRemovedMainThread), new DemuxerPadInfo(demuxersrcpad, ap));
}

static gboolean appendPipelineDemuxerPadRemovedMainThread(DemuxerPadInfo* info)
{
    if (info->ap()->qtdemux())
        info->ap()->demuxerPadRemoved(info->pad());
    delete info;
    return G_SOURCE_REMOVE;
}

static void appendPipelineAppSinkCapsChanged(GObject*, GParamSpec*, AppendPipeline* ap)
{
    if (WTF::isMainThread())
        ap->appSinkCapsChanged();
    else {
        ap->ref();
        g_timeout_add(0, GSourceFunc(appendPipelineAppSinkCapsChangedMainThread), ap);
    }
}

static gboolean appendPipelineAppSinkCapsChangedMainThread(AppendPipeline* ap)
{
    ap->appSinkCapsChanged();
    ap->deref();
    return G_SOURCE_REMOVE;
}

static void appendPipelineAppSinkNewSample(GstElement* appsink, AppendPipeline* ap)
{
    // Done in the streaming thread for performance.
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));

    if (WTF::isMainThread())
        ap->appSinkNewSample(sample);
    else {
        g_timeout_add(0, GSourceFunc(appendPipelineAppSinkNewSampleMainThread), new NewSampleInfo(sample, ap));
    }
    gst_sample_unref(sample);
}

static gboolean appendPipelineAppSinkNewSampleMainThread(NewSampleInfo* info)
{
    info->ap()->appSinkNewSample(info->sample());
    delete info;
    return G_SOURCE_REMOVE;
}

static void appendPipelineAppSinkEOS(GstElement*, AppendPipeline* ap)
{
    if (WTF::isMainThread())
        ap->appSinkEOS();
    else {
        ap->ref();
        g_timeout_add(0, GSourceFunc(appendPipelineAppSinkEOSMainThread), ap);
    }

    printf("### %s: %s main thread\n", __PRETTY_FUNCTION__, (WTF::isMainThread())?"IS":"NOT"); fflush(stdout);
}

static gboolean appendPipelineAppSinkEOSMainThread(AppendPipeline* ap)
{
    ap->appSinkEOS();
    ap->deref();
    return G_SOURCE_REMOVE;
}

static gboolean appendPipelineNoDataToDecodeTimeout(AppendPipeline* ap)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    ap->setAppendStage(AppendPipeline::NoDataToDecode);
    return G_SOURCE_REMOVE;
}

static gboolean appendPipelineLastSampleTimeout(AppendPipeline* ap)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    ap->setAppendStage(AppendPipeline::LastSample);
    return G_SOURCE_REMOVE;
}

PassRefPtr<MediaSourceClientGStreamerMSE> MediaSourceClientGStreamerMSE::create(PassRefPtr<MediaPlayerPrivateGStreamerMSE> playerPrivate)
{
    return adoptRef(new MediaSourceClientGStreamerMSE(playerPrivate));
}

MediaSourceClientGStreamerMSE::MediaSourceClientGStreamerMSE(PassRefPtr<MediaPlayerPrivateGStreamerMSE> playerPrivate)
    : RefCounted<MediaSourceClientGStreamerMSE>()
    , m_playerPrivate(playerPrivate)
    , m_duration(MediaTime::invalidTime())
{
}

MediaSourceClientGStreamerMSE::~MediaSourceClientGStreamerMSE()
{
    // TODO: cancel m_noDataToDecodeTimeoutTag if active and perform appendComplete()
}

MediaSourcePrivate::AddStatus MediaSourceClientGStreamerMSE::addSourceBuffer(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, const ContentType&)
{
    RefPtr<AppendPipeline> ap = adoptRef(new AppendPipeline(this, sourceBufferPrivate, m_playerPrivate.get()));
    printf("### %s: this=%p sourceBuffer=%p ap=%p\n", __PRETTY_FUNCTION__, this, sourceBufferPrivate.get(), ap.get()); fflush(stdout);
    m_playerPrivate->m_appendPipelinesMap.add(sourceBufferPrivate, ap);
    if (!m_playerPrivate->mediaSourceClient())
        m_playerPrivate->setMediaSourceClient(ap->mediaSourceClient());

    ASSERT(m_playerPrivate->m_playbackPipeline);

    return m_playerPrivate->m_playbackPipeline->addSourceBuffer(sourceBufferPrivate);
}

MediaTime MediaSourceClientGStreamerMSE::duration()
{
    return m_duration;
}

void MediaSourceClientGStreamerMSE::durationChanged(const MediaTime& duration)
{
    printf("### %s: duration=%f\n", __PRETTY_FUNCTION__, duration.toFloat()); fflush(stdout);

    if (!duration.isValid() || duration.isPositiveInfinite() || duration.isNegativeInfinite())
        return;

    m_duration = duration;
    m_playerPrivate->durationChanged();

    // TODO: Maybe convert to GstClockTime and emit duration changed on the appsrc, like this:
    // gst_element_post_message(GST_ELEMENT(m_src.get()), gst_message_new_duration_changed(GST_OBJECT(m_src.get())));
}

void MediaSourceClientGStreamerMSE::abort(PassRefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);

    RefPtr<AppendPipeline> ap = m_playerPrivate->m_appendPipelinesMap.get(sourceBufferPrivate);
    ap->abort();
}

bool MediaSourceClientGStreamerMSE::append(PassRefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate, const unsigned char* data, unsigned length)
{
    printf("### %s: %u bytes\n", __PRETTY_FUNCTION__, length); fflush(stdout);

    RefPtr<AppendPipeline> ap = m_playerPrivate->m_appendPipelinesMap.get(sourceBufferPrivate);
    GstBuffer* buffer = gst_buffer_new_and_alloc(length);
    gst_buffer_fill(buffer, 0, data, length);
    ap->setAppendStage(AppendPipeline::Ongoing);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(ap->appsrc()), buffer);

    return (ret == GST_FLOW_OK);
}

void MediaSourceClientGStreamerMSE::appendComplete(SourceBufferPrivateClient::AppendResult)
{
    // TODO
}

void MediaSourceClientGStreamerMSE::markEndOfStream(MediaSourcePrivate::EndOfStreamStatus)
{
    // TODO
}

void MediaSourceClientGStreamerMSE::removedFromMediaSource(RefPtr<SourceBufferPrivateGStreamer> sourceBufferPrivate)
{
    RefPtr<AppendPipeline> ap = m_playerPrivate->m_appendPipelinesMap.get(sourceBufferPrivate);
    m_playerPrivate->m_appendPipelinesMap.remove(sourceBufferPrivate);
    gst_element_set_state (ap->pipeline(), GST_STATE_NULL);
    if (m_playerPrivate->m_appendPipelinesMap.isEmpty())
        m_playerPrivate->setMediaSourceClient(RefPtr<MediaSourceClientGStreamerMSE>(this));
    // AppendPipeline destructor will take care of cleaning up when appropriate.

    ASSERT(m_playerPrivate->m_playbackPipeline);

    m_playerPrivate->m_playbackPipeline->removeSourceBuffer(sourceBufferPrivate);
}

void MediaSourceClientGStreamerMSE::flushAndEnqueueNonDisplayingSamples(Vector<RefPtr<MediaSample> > samples, AtomicString trackIDString)
{
    m_playerPrivate->m_playbackPipeline->flushAndEnqueueNonDisplayingSamples(samples);
}

void MediaSourceClientGStreamerMSE::enqueueSample(PassRefPtr<MediaSample> prsample, AtomicString trackIDString)
{
    m_playerPrivate->m_playbackPipeline->enqueueSample(prsample);
}

void MediaSourceClientGStreamerMSE::didReceiveInitializationSegment(SourceBufferPrivateGStreamer* sourceBuffer, const SourceBufferPrivateClient::InitializationSegment& initializationSegment)
{
    sourceBuffer->didReceiveInitializationSegment(initializationSegment);
}

void MediaSourceClientGStreamerMSE::didReceiveSample(SourceBufferPrivateGStreamer* sourceBuffer, PassRefPtr<MediaSample> sample)
{
    sourceBuffer->didReceiveSample(sample);
}

void MediaSourceClientGStreamerMSE::didReceiveAllPendingSamples(SourceBufferPrivateGStreamer* sourceBuffer)
{
    printf("### %s\n", __PRETTY_FUNCTION__); fflush(stdout);
    sourceBuffer->didReceiveAllPendingSamples();
}

GRefPtr<WebKitMediaSrc> MediaSourceClientGStreamerMSE::webKitMediaSrc()
{
    WebKitMediaSrc* source = WEBKIT_MEDIA_SRC(m_playerPrivate->m_source.get());

    ASSERT(WEBKIT_IS_MEDIA_SRC(source));

    return GRefPtr<WebKitMediaSrc>(source);
}

} // namespace WebCore

#endif // USE(GSTREAMER)
