/*
 * Copyright (C) 2001 Apple Computer, Inc.  All rights reserved.
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
#ifndef KWQMETRICS_H_
#define KWQMETRICS_H_

#import <Cocoa/Cocoa.h>

@interface KWQLayoutInfo : NSObject
{
    NSMutableDictionary *attributes;
    NSMutableDictionary *fragmentCache;
}

+ (void)drawString: (NSString *)string atPoint: (NSPoint)p withFont: (NSFont *)font color: (NSColor *)color;
+ (void)drawUnderlineForString: (NSString *)string atPoint: (NSPoint)p withFont: (NSFont *)font color: (NSColor *)color;
+ (KWQLayoutInfo *)getMetricsForFont: (NSFont *)aFont;
+ (void)setMetric: (KWQLayoutInfo *)info forFont: (NSFont *)aFont;
- initWithFont: (NSFont *)aFont;
- (NSRect)rectForString:(NSString *)string;
- (NSLayoutManager *)layoutManagerForString: (NSString *)string;
- (void)setColor: (NSColor *)color;
- (void)setFont: (NSFont *)aFont;
- (NSDictionary *)attributes;
@end

@interface KWQLayoutFragment : NSObject
{
    NSTextStorage *textStorage;
    NSTextContainer *textContainer;
    NSLayoutManager *layoutManager;
    NSRect boundingRect;
    BOOL cachedRect;
}

- initWithString: (NSString *)storage attributes: (NSDictionary *)attrs;
- (NSRect)boundingRect;
- (void)dealloc;

@end

#endif
