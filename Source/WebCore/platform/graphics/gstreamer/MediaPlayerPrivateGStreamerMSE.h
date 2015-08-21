/*
 * Copyright (C) 2007, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2007 Collabora Ltd. All rights reserved.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2009, 2010 Igalia S.L
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

#ifndef MediaPlayerPrivateGStreamerMSE_h
#define MediaPlayerPrivateGStreamerMSE_h
#if ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(MEDIA_SOURCE) && ENABLE(VIDEO_TRACK)

#include "GRefPtrGStreamer.h"
#include "MediaPlayerPrivateGStreamerBase.h"
#include "Timer.h"

#include <glib.h>
#include <gst/gst.h>
#include <gst/pbutils/install-plugins.h>
#include <wtf/Forward.h>
#include <wtf/glib/GSourceWrap.h>
#include <wtf/HashMap.h>
#include <wtf/PassRefPtr.h>
#include <wtf/RefPtr.h>

#if USE(GSTREAMER_MPEGTS)
#include <wtf/text/AtomicStringHash.h>
#endif

#include "MediaSourceGStreamer.h"
#include "WebKitMediaSourceGStreamer.h"

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
#include <wtf/threads/BinarySemaphore.h>
#endif

typedef struct _GstBuffer GstBuffer;
typedef struct _GstMessage GstMessage;
typedef struct _GstElement GstElement;
typedef struct _GstMpegtsSection GstMpegtsSection;

namespace WebCore {

#if ENABLE(WEB_AUDIO)
class AudioSourceProvider;
class AudioSourceProviderGStreamer;
#endif

#if ENABLE(ENCRYPTED_MEDIA) && USE(DXDRM)
class DiscretixSession;
#endif

class AudioTrackPrivateGStreamer;
class InbandMetadataTextTrackPrivateGStreamer;
class InbandTextTrackPrivateGStreamer;
class MediaPlayerRequestInstallMissingPluginsCallback;
class VideoTrackPrivateGStreamer;
class MediaSourceClientGStreamerMSE;

class MediaPlayerPrivateGStreamerMSE : public MediaPlayerPrivateGStreamerBase, public RefCounted<MediaPlayerPrivateGStreamerMSE> {
    WTF_MAKE_NONCOPYABLE(MediaPlayerPrivateGStreamerMSE); WTF_MAKE_FAST_ALLOCATED;

    friend class MediaSourceClientGStreamerMSE;

public:
    explicit MediaPlayerPrivateGStreamerMSE(MediaPlayer*);
    ~MediaPlayerPrivateGStreamerMSE();

    static void registerMediaEngine(MediaEngineRegistrar);
    void handleSyncMessage(GstMessage*);
    gboolean handleMessage(GstMessage*);
    void handlePluginInstallerResult(GstInstallPluginsReturn);

    bool hasVideo() const override { return m_hasVideo; }
    bool hasAudio() const override { return m_hasAudio; }

    void load(const String &url) override;
    void load(const String& url, MediaSourcePrivateClient*) override;
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) override;
#endif
    void commitLoad();
    void cancelLoad() override;

    void prepareToPlay() override;
    void play() override;
    void pause() override;

    bool paused() const override;
    bool seeking() const override;

    float duration() const override;
    float currentTime() const override;
    void seek(float) override;

    void setReadyState(MediaPlayer::ReadyState state) override;
    void waitForSeekCompleted() override;
    void seekCompleted() override;
    MediaSourcePrivateClient* mediaSourcePrivateClient() { return m_mediaSource.get(); }

    void setRate(float) override;
    double rate() const override;
    void setPreservesPitch(bool) override;

    void setPreload(MediaPlayer::Preload) override;
    void fillTimerFired();

    std::unique_ptr<PlatformTimeRanges> buffered() const override;
    float maxTimeSeekable() const override;
    bool didLoadingProgress() const override;
    unsigned long long totalBytes() const override;
    float maxTimeLoaded() const override;

    void loadStateChanged();
    void timeChanged();
    void didEnd();
    void notifyDurationChanged();
    void durationChanged();
    void loadingFailed(MediaPlayer::NetworkState);

    void videoChanged();
    void videoCapsChanged();
    void audioChanged();
    void notifyPlayerOfVideo();
    void notifyPlayerOfVideoCaps();
    void notifyPlayerOfAudio();

    void textChanged();
    void notifyPlayerOfText();

    void newTextSample();
    void notifyPlayerOfNewTextSample();

    void sourceChanged();
    GstElement* audioSink() const override;
    void configurePlaySink();

    void setAudioStreamProperties(GObject*);

    void simulateAudioInterruption() override;

    bool changePipelineState(GstState);

#if ENABLE(WEB_AUDIO)
    AudioSourceProvider* audioSourceProvider() override { return reinterpret_cast<AudioSourceProvider*>(m_audioSourceProvider.get()); }
#endif

#if ENABLE(ENCRYPTED_MEDIA)
    MediaPlayer::MediaKeyException addKey(const String&, const unsigned char*, unsigned, const unsigned char*, unsigned, const String&);
    MediaPlayer::MediaKeyException generateKeyRequest(const String&, const unsigned char*, unsigned);
    MediaPlayer::MediaKeyException cancelKeyRequest(const String&, const String&);
    void needKey(const String&, const String&, const unsigned char*, unsigned);
#endif
#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    void signalDRM();
#endif

    bool isLiveStream() const override { return m_isStreaming; }
    void notifyAppendComplete();

#if ENABLE(ENCRYPTED_MEDIA_V2)
    void needKey(RefPtr<Uint8Array> initData);
    void setCDMSession(CDMSession*);
    void keyAdded();
#endif

    using RefCounted<MediaPlayerPrivateGStreamerMSE>::ref;
    using RefCounted<MediaPlayerPrivateGStreamerMSE>::deref;

private:
    static void getSupportedTypes(HashSet<String>&);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    static bool isAvailable();
    static bool supportsKeySystem(const String& keySystem, const String& mimeType);

    GstElement* createAudioSink() override;

#if ENABLE(ENCRYPTED_MEDIA_V2)
    std::unique_ptr<CDMSession> createSession(const String&);
    CDMSession* m_cdmSession;
#endif

#if ENABLE(ENCRYPTED_MEDIA) && USE(DXDRM)
    DiscretixSession* m_dxdrmSession;
#endif

    float playbackPosition() const;

    void cacheDuration();
    void updateStates();
    void asyncStateChangeDone();

    void createGSTPlayBin();

    bool loadNextLocation();
    void mediaLocationChanged(GstMessage*);

    void setDownloadBuffering();
    void processBufferingStats(GstMessage*);
#if USE(GSTREAMER_MPEGTS)
    void processMpegTsSection(GstMpegtsSection*);
#endif
    void processTableOfContents(GstMessage*);
    void processTableOfContentsEntry(GstTocEntry*, GstTocEntry* parent);
    bool doSeek(gint64 position, float rate, GstSeekFlags seekType);
    void updatePlaybackRate();


    String engineDescription() const override { return "GStreamer"; }
    bool didPassCORSAccessCheck() const override;
    bool canSaveMediaData() const override;

    // TODO: Implement
    unsigned long totalVideoFrames() override { return 0; }
    unsigned long droppedVideoFrames() override { return 0; }
    unsigned long corruptedVideoFrames() override { return 0; }
    MediaTime totalFrameDelay() override { return MediaTime::zeroTime(); }
    GRefPtr<GstCaps> currentDemuxerCaps() const override;
    bool timeIsBuffered(float);

private:
    GRefPtr<GstElement> m_source;
    GRefPtr<GstElement> m_textAppSink;
    GRefPtr<GstPad> m_textAppSinkPad;
    float m_seekTime;
    bool m_changingRate;
    float m_endTime;
    mutable bool m_isStreaming;
    GstStructure* m_mediaLocations;
    int m_mediaLocationCurrentIndex;
    bool m_resetPipeline;
    bool m_paused;
    bool m_playbackRatePause;
    bool m_seeking;
    bool m_seekIsPending;
    float m_timeOfOverlappingSeek;
    bool m_canFallBackToLastFinishedSeekPosition;
    bool m_buffering;
    float m_playbackRate;
    float m_lastPlaybackRate;
    bool m_errorOccured;
    mutable gfloat m_mediaDuration;
    bool m_downloadFinished;
    Timer m_fillTimer;
    float m_maxTimeLoaded;
    int m_bufferingPercentage;
    MediaPlayer::Preload m_preload;
    bool m_delayingLoad;
    bool m_mediaDurationKnown;
    mutable float m_maxTimeLoadedAtLastDidLoadingProgress;
    bool m_volumeAndMuteInitialized;
    bool m_hasVideo;
    bool m_hasAudio;
    GSourceWrap::Static m_audioTimerHandler;
    GSourceWrap::Static m_textTimerHandler;
    GSourceWrap::Static m_videoTimerHandler;
    GSourceWrap::Static m_videoCapsTimerHandler;
    GSourceWrap::Static m_readyTimerHandler;
    mutable unsigned long long m_totalBytes;
    URL m_url;
    bool m_preservesPitch;
    mutable float m_cachedPosition;
    mutable double m_lastQuery;
#if ENABLE(WEB_AUDIO)
    std::unique_ptr<AudioSourceProviderGStreamer> m_audioSourceProvider;
#endif
    GstState m_requestedState;
    GRefPtr<GstElement> m_autoAudioSink;
    RefPtr<MediaPlayerRequestInstallMissingPluginsCallback> m_missingPluginsCallback;
    Vector<RefPtr<AudioTrackPrivateGStreamer>> m_audioTracks;
    Vector<RefPtr<InbandTextTrackPrivateGStreamer>> m_textTracks;
    Vector<RefPtr<VideoTrackPrivateGStreamer>> m_videoTracks;
    RefPtr<InbandMetadataTextTrackPrivateGStreamer> m_chaptersTrack;
#if USE(GSTREAMER_MPEGTS)
    HashMap<AtomicString, RefPtr<InbandMetadataTextTrackPrivateGStreamer>> m_metadataTracks;
#endif
    RefPtr<MediaSourcePrivateClient> m_mediaSource;
    bool isMediaSource() const { return m_mediaSource && WEBKIT_IS_MEDIA_SRC(m_source.get()); }
#if USE(GSTREAMER_GL)
    GstGLContext* m_glContext;
    GstGLDisplay* m_glDisplay;
#endif
#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    BinarySemaphore m_drmKeySemaphore;
#endif
    Mutex m_pendingAsyncOperationsLock;
    GList* m_pendingAsyncOperations;
    bool m_seekCompleted;

    HashMap<RefPtr<SourceBufferPrivateGStreamer>, GstElement* > m_appendPipelinesMap;
};

class ContentType;
class SourceBufferPrivateGStreamer;

class MediaSourceClientGStreamerMSE: public RefCounted<MediaSourceClientGStreamerMSE> {
    public:
        static PassRefPtr<MediaSourceClientGStreamerMSE> create(PassRefPtr<MediaPlayerPrivateGStreamerMSE> playerPrivate);
        virtual ~MediaSourceClientGStreamerMSE();

        // From MediaSourceGStreamer
        MediaSourcePrivate::AddStatus addSourceBuffer(PassRefPtr<SourceBufferPrivateGStreamer>, const ContentType&);
        void durationChanged(const MediaTime&);
        void markEndOfStream(MediaSourcePrivate::EndOfStreamStatus);

        // From SourceBufferPrivateGStreamer
        bool append(PassRefPtr<SourceBufferPrivateGStreamer>, const unsigned char*, unsigned);
        void appendComplete(SourceBufferPrivateClient::AppendResult);
        void removedFromMediaSource(PassRefPtr<SourceBufferPrivateGStreamer>);
        void flushAndEnqueueNonDisplayingSamples(Vector<RefPtr<MediaSample> > samples, AtomicString trackIDString);
        void enqueueSample(PassRefPtr<MediaSample> sample, AtomicString trackIDString);

        // From our WebKitMediaSrc
        void didReceiveInitializationSegment(SourceBufferPrivateGStreamer*, const SourceBufferPrivateClient::InitializationSegment&);
        void didReceiveSample(SourceBufferPrivateGStreamer* sourceBuffer, PassRefPtr<MediaSample> sample);
        void didReceiveAllPendingSamples(SourceBufferPrivateGStreamer* sourceBuffer);

    private:
        MediaSourceClientGStreamerMSE(PassRefPtr<MediaPlayerPrivateGStreamerMSE> playerPrivate);

        RefPtr<MediaPlayerPrivateGStreamerMSE> m_playerPrivate;
};

} // namespace WebCore

#endif // USE(GSTREAMER)
#endif
