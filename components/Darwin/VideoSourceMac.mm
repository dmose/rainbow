/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Rainbow.
 *
 * The Initial Developer of the Original Code is Mozilla Labs.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Anant Narayanan <anant@kix.in>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#import <QTKit/QTkit.h>
#include "VideoSourceMac.h"

/* Objective-C Implementation here */
@interface MozQTCapture : NSObject {
    QTCaptureSession *mSession;
    QTCaptureDeviceInput *mVideo;
    QTCaptureDecompressedVideoOutput *mOutput;
    CVPixelBufferRef mCurrentFrame;
    
    BOOL rec;
    FILE *tmp;
    nsIOutputStream *output;
    nsIDOMCanvasRenderingContext2D *vCanvas;
}

- (BOOL)start:(nsIOutputStream *)pipe
    withCanvas:(nsIDOMCanvasRenderingContext2D *)ctx
    width:(int)w andHeight:(int)h;
- (BOOL)stop;
- (void)processFrames;
- (void)captureOutput:(QTCaptureOutput *)captureOutput
    didOutputVideoFrame:(CVImageBufferRef)frame
    withSampleBuffer:(QTSampleBuffer*)sampleBuffer
    fromConnection:(QTCaptureConnection *)connection;

@end

@implementation MozQTCapture

- (BOOL)start:(nsIOutputStream *)pipe
    withCanvas:(nsIDOMCanvasRenderingContext2D *)ctx
    width:(int)w andHeight:(int)h
{
    NSError *error;
    BOOL success = NO;
    mSession = [[QTCaptureSession alloc] init];
    
    QTCaptureDevice *video =
        [QTCaptureDevice defaultInputDeviceWithMediaType:QTMediaTypeVideo];
    success = [video open:&error];
    
    if (!success) {
        NSLog(@"Could not acquire device!\n");
        return NO;
    }
    
    NSLog(@"Acquired %@", [video localizedDisplayName]);

    mVideo = [[QTCaptureDeviceInput alloc] initWithDevice:video];
    success = [mSession addInput:mVideo error:&error];
    
    if (!success) {
        NSLog(@"Could not add input to session!");
        return NO;
    }
    
    mOutput = [[QTCaptureDecompressedVideoOutput alloc] init];
    [mOutput setDelegate:self];
    
    NSDictionary *attributes = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithDouble:w], (id)kCVPixelBufferWidthKey,
        [NSNumber numberWithDouble:h], (id)kCVPixelBufferHeightKey,
        [NSNumber numberWithUnsignedInt:kCVPixelFormatType_420YpCbCr8Planar],
        //[NSNumber numberWithUnsignedInt:kCVPixelFormatType_422YpCbCr8],
        (id)kCVPixelBufferPixelFormatTypeKey,
        nil];
    [mOutput setPixelBufferAttributes:attributes];

    success = [mSession addOutput:mOutput error:&error];
    if (!success) {
        NSLog(@"Could not add output to session!");
        return NO;
    }
    
    output = pipe;
    vCanvas = ctx;

    tmp = fopen("/Users/anant/Code/tmp/qtkit/test.raw", "w+");
    [mSession startRunning];
    rec = YES;
    [NSThread detachNewThreadSelector:@selector(processFrames)
        toTarget:self withObject:nil];
    
    NSLog(@"Began session %d!", [mSession isRunning]);
    return YES;
}

- (BOOL)stop
{
    [mSession stopRunning];
    rec = NO;
    fclose(tmp);

    if ([[mVideo device] isOpen])
        [[mVideo device] close];
    
    NSLog(@"Ended session %d!", [mSession isRunning]);
    return YES;
}

- (void)dealloc
{
    [mSession release];
    [mVideo release];
    [mOutput release];
    
    [super dealloc];
}

- (void)captureOutput:(QTCaptureOutput *)captureOutput
    didOutputVideoFrame:(CVImageBufferRef)videoFrame
    withSampleBuffer:(QTSampleBuffer *)sampleBuffer
    fromConnection:(QTCaptureConnection *)connection
{
    NSLog(@"CALLBACK!!!! %@", [NSRunLoop currentRunLoop]);
    
    CVPixelBufferRef toRelease;
    CVBufferRetain(videoFrame);
    
    @synchronized (self) {
        toRelease = mCurrentFrame;
        mCurrentFrame = videoFrame;
    }
    
    CVBufferRelease(toRelease);
}

-(void)processFrames
{
    while (rec) {
        CVPixelBufferRef frame;
        @synchronized (self) {
            frame = CVBufferRetain(mCurrentFrame);
        }
        
        if (frame) {
            CVPixelBufferLockBaseAddress(frame, 0);
            void *addr;
            nsresult rv;
            PRUint32 wr;
            size_t l, r, t, b, row, cx;

            CVPixelBufferGetExtendedPixels(frame, &l, &r, &t, &b);
   
            row = CVPixelBufferGetBytesPerRow(frame);
            cx = CVPixelBufferGetPlaneCount(frame);
            size_t w = CVPixelBufferGetWidth(frame);
            size_t h = CVPixelBufferGetHeight(frame);

            //NSLog(@"THREAD!!!! %@", [NSRunLoop currentRunLoop]);
            //NSLog(@"w:%d h:%d l:%d r:%d t:%d b:%d r:%d c:%d\n", w, h, l, r, t, b, row, cx);

            int fsize = w * h * 4;
            int isize = (w * h * 3) / 2;

            /* Planar i420 frame. Start from Y plane and upto isize bytes */
            addr = CVPixelBufferGetBaseAddressOfPlane(frame, 0);
            fwrite(addr, isize, 1, tmp);

            CVPixelBufferUnlockBaseAddress(frame, 0);
            CVBufferRelease(frame);

            /* Write to pipe */
            rv = output->Write(
                (const char *)addr, isize, &wr
            );

            /* Write to canvas, if needed */
            if (vCanvas) {
                nsAutoArrayPtr<PRUint8> rgb32(new PRUint8[fsize]);
                I420toRGB32(w, h, (const char *)addr, (char *)rgb32.get());

                nsCOMPtr<nsIRunnable> render = new CanvasRenderer(
                    vCanvas, w, h, rgb32, fsize
                );
                rv = NS_DispatchToMainThread(render);
            }
        }
    }
}

@end

/* C++ wrapper */
VideoSourceMac::VideoSourceMac(int w, int h)
    : VideoSource(w, h)
{
    fps_n = 15;
    fps_d = 1;
    
    pool = [[NSAutoreleasePool alloc] init];
    objc = [[MozQTCapture alloc] init];
    g2g = PR_TRUE;
}

VideoSourceMac::~VideoSourceMac()
{
    [(id)objc release];
    [(id)pool release];
}

nsresult
VideoSourceMac::Start(
    nsIOutputStream *pipe, nsIDOMCanvasRenderingContext2D *ctx)
{
    if (!g2g)
        return NS_ERROR_FAILURE;

    MozQTCapture *mqtc = (id)objc;
    if ([mqtc start:pipe withCanvas:ctx width:width andHeight:height]) {
        NSLog(@"Started MozQTCapture!");
        return NS_OK;
    } else {
        return NS_ERROR_FAILURE;
    }
}

nsresult
VideoSourceMac::Stop()
{
    if (!g2g)
        return NS_ERROR_FAILURE;

    MozQTCapture *mqtc = (id)objc;
    if ([mqtc stop]) {
        return NS_OK;
    }
    return NS_ERROR_FAILURE;
}

