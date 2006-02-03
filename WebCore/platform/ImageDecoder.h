/*
 * Copyright (C) 2004, 2005, 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef IMAGE_DECODER_H_
#define IMAGE_DECODER_H_

#if __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace WebCore {

class IntSize;

template <typename T> class Array;
typedef Array<char> ByteArray;

#if __APPLE__
typedef CGImageSourceRef NativeImageDecoderPtr;
typedef CGImageRef NativeImagePtr;
typedef CFDataRef NativeBytePtr;
#else
class ImageDecoderPlugin;
typedef ImageDecoderPlugin* NativeImageDecoderPtr;
typedef ByteArray* NativeBytePtr;
#endif

const int cAnimationLoopOnce = -1;
const int cAnimationNone = -2;

class ImageDecoder {
public:
    ImageDecoder();
    ~ImageDecoder();

    bool initialized() const;
    
    void setData(NativeBytePtr data, bool allDataReceived);

    bool isSizeAvailable();
    IntSize size() const;
    
    int repetitionCount();
    
    size_t frameCount() const;
    NativeImagePtr createFrameAtIndex(size_t index);
    float frameDurationAtIndex(size_t index);
    
private:
    NativeImageDecoderPtr m_decoder;
};

}

#endif
