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
 * The Original Code is Video for Jetpack.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Labs
 * Portions created by the Initial Developer are Copyright (C) 2009
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

#include "VideoRecorder.h"

NS_IMPL_ISUPPORTS1(VideoRecorder, IVideoRecorder)

VideoRecorder *VideoRecorder::gVideoRecordingService = nsnull;

VideoRecorder *
VideoRecorder::GetSingleton()
{
    if (gVideoRecordingService) {
        gVideoRecordingService->AddRef();
        return gVideoRecordingService;
    }
    
    gVideoRecordingService = new VideoRecorder();
    if (gVideoRecordingService) {
        gVideoRecordingService->AddRef();
        if (NS_FAILED(gVideoRecordingService->Init()))
            gVideoRecordingService->Release();
    }
    
    return gVideoRecordingService;
}

nsresult
VideoRecorder::Init()
{
    recording = 0;
    int num_devices = 0;
    struct vidcap_sapi_info sapi_info;
    
    if (!(state = vidcap_initialize())) {
        fprintf(stderr, "Could not initialize vidcap, aborting!\n");
        return NS_ERROR_FAILURE;
    }
    
    if (!(sapi = vidcap_sapi_acquire(state, 0))) {
        fprintf(stderr, "Failed to acquire default sapi\n");
        return NS_ERROR_FAILURE;
    }
    
    if (vidcap_sapi_info_get(sapi, &sapi_info)) {
        fprintf(stderr, "Failed to get default sapi info\n");
        return NS_ERROR_FAILURE;
    }
    
    num_devices = vidcap_src_list_update(sapi);
    if (num_devices < 0) {
        fprintf(stderr, "Failed vidcap_src_list_update()\n");
        return NS_ERROR_FAILURE;
    } else if (num_devices == 0) {
        /* FIXME: Not really a failure */
        fprintf(stderr, "No video capture sources available\n");
        return NS_ERROR_FAILURE;
    }
    
    if (!(sources = (struct vidcap_src_info *)
        PR_Calloc(num_devices, sizeof(struct vidcap_src_info)))) {
        return NS_ERROR_OUT_OF_MEMORY;
    }
    
    if (vidcap_src_list_get(sapi, num_devices, sources)) {
        PR_Free(sources);
        fprintf(stderr, "Failed vidcap_src_list_get()\n");
        return NS_ERROR_FAILURE;
    }
        
    size = WIDTH * HEIGHT * 3 / 2;
    ogg_state = (ogg_stream_state *)PR_Calloc(1, sizeof(ogg_stream_state));
    
    /* Setup our pipe */
    nsCOMPtr<nsIPipe> pipe = do_CreateInstance("@mozilla.org/pipe;1");
    if (!pipe)
        return NS_ERROR_OUT_OF_MEMORY;
    nsresult rv = pipe->Init(
        PR_FALSE, PR_FALSE,
        size, 128, nsnull
    );
    if (NS_FAILED(rv))
        return rv;

    pipe->GetInputStream(getter_AddRefs(mPipeIn));
    pipe->GetOutputStream(getter_AddRefs(mPipeOut));
    return NS_OK;
}

VideoRecorder::~VideoRecorder()
{
    vidcap_sapi_release(sapi);
    vidcap_destroy(state);
    PR_Free(sources);
    PR_Free(ogg_state);
    gVideoRecordingService = nsnull;
}

#define TABLE_SIZE 36
static const char table[] = {
    'a','b','c','d','e','f','g','h','i','j',
    'k','l','m','n','o','p','q','r','s','t',
    'u','v','w','x','y','z','0','1','2','3',
    '4','5','6','7','8','9' 
};

/*
 * This code is ripped from profile/src/nsProfile.cpp and is further
 * duplicated in uriloader/exthandler.  this should probably be moved
 * into xpcom or some other shared library.
 */ 
static void
MakeRandomString(char *buf, PRInt32 bufLen)
{
    /* turn PR_Now() into milliseconds since epoch
     * and salt rand with that.
     */
    double fpTime;
    LL_L2D(fpTime, PR_Now());

    /* use 1e-6, granularity of PR_Now() on the mac is seconds */
    srand((uint)(fpTime * 1e-6 + 0.5));   
    PRInt32 i;
    for (i=0;i<bufLen;i++) {
        *buf++ = table[rand()%TABLE_SIZE];
    }
    *buf = 0;
}

/*
 * This replaces \ with \\ so that Windows paths are sane
 */
static void
EscapeBackslash(nsACString& str)
{
    const char *sp;
    const char *mp = "\\";
    const char *np = "\\\\";

    PRUint32 sl;
    PRUint32 ml = 1;
    PRUint32 nl = 2;

    sl = NS_CStringGetData(str, &sp);
    for (const char* iter = sp; iter <= sp + sl - ml; ++iter) {
        if (memcmp(iter, mp, ml) == 0) {
            PRUint32 offset = iter - sp;
            NS_CStringSetDataRange(str, offset, ml, np, nl);
            sl = NS_CStringGetData(str, &sp);
            iter = sp + offset + nl - 1;
        }
    }
}

void
VideoRecorder::Encode(void *data)
{
    nsresult rv;
    PRUint32 rd;
    ogg_page og;
    ogg_packet op;
    th_ycbcr_buffer ycbcr;
    
    VideoRecorder *vr = static_cast<VideoRecorder*>(data);
    unsigned char *frame = (unsigned char *)
        PR_Calloc(1, vr->size);
    
    for (;;) {
        rv = vr->mPipeIn->Read((char *)frame, vr->size, &rd);
        if (rd == 0) {
            /* EOF */
            return;
        } else if (rd != (PRUint32) vr->size) {
            fprintf(stderr, "Could only read %d from pipe!\n", rd);
            return;
        }

        ycbcr[0].width = WIDTH;
        ycbcr[0].stride = WIDTH;
        ycbcr[0].height = HEIGHT;

        ycbcr[1].width = (WIDTH >> 1);
        ycbcr[1].height = (HEIGHT >> 1);
        ycbcr[1].stride = ycbcr[1].width;

        ycbcr[2].width = ycbcr[1].width;
        ycbcr[2].height = ycbcr[1].height;
        ycbcr[2].stride = ycbcr[1].stride;

        ycbcr[0].data = frame;
        ycbcr[1].data = frame + WIDTH * HEIGHT;
        ycbcr[2].data = ycbcr[1].data + WIDTH * HEIGHT / 4;

        if (th_encode_ycbcr_in(vr->encoder, ycbcr) != 0) {
            fprintf(stderr, "Could not encode frame!\n");
            return;
        }
        if (!th_encode_packetout(vr->encoder, 0, &op)) {
            fprintf(stderr, "Could not read packet!\n");
            return;
        }

        ogg_stream_packetin(vr->ogg_state, &op);
        while (ogg_stream_pageout(vr->ogg_state, &og)) {
            fwrite(og.header, og.header_len, 1, vr->outfile);
            fwrite(og.body, og.body_len, 1, vr->outfile);
        }
    }
}

int
VideoRecorder::Callback(vidcap_src *src, void *data,
    struct vidcap_capture_info *video)
{
    int frames;
    nsresult rv;
    PRUint32 wr;
    VideoRecorder *vr = static_cast<VideoRecorder*>(data);
    
    /* Write to pipe, paint to canvas, and return quickly */
    rv = vr->mPipeOut->Write(
        (const char *)video->video_data,
        video->video_data_size, &wr
    );
    
    if (!vr->mCtx) {
        return 0;
    }

    frames = video->video_data_size / vr->size;
    unsigned char *yuv = (unsigned char *)video->video_data;

    for (int i = 0; i < frames; i++) {
        unsigned char *rgb = (unsigned char *)
            PR_Calloc(1, WIDTH * HEIGHT * 4);
        vidcap_i420_to_rgb32(
            WIDTH, HEIGHT,
            (const char *)yuv, (char *)rgb
        );
        rv = vr->mCtx->PutImageData_explicit(
            0, 0, WIDTH, HEIGHT, rgb, WIDTH * HEIGHT * 4
        );
        PR_Free((void *)rgb);
        yuv += vr->size;
    }
    return 0;
}

/*
 * Setup Ogg/Theora file
 */
nsresult
VideoRecorder::SetupOggTheora(nsACString& file)
{
    int ret;
    th_info ti;
    nsresult rv;
    char buf[13];
    th_comment tc;
    ogg_page page;
    ogg_packet packet;
    nsCAutoString path;
    nsCOMPtr<nsIFile> o;
    
    /* Assign temporary name */
    rv = NS_GetSpecialDirectory(NS_OS_TEMP_DIR, getter_AddRefs(o));
    if (NS_FAILED(rv)) return rv;
    
    MakeRandomString(buf, 8);
    memcpy(buf + 8, ".ogg", 5);
    rv = o->AppendNative(nsDependentCString(buf, 12));
    if (NS_FAILED(rv)) return rv;
    rv = o->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600);
    if (NS_FAILED(rv)) return rv;
    rv = o->GetNativePath(path);
    if (NS_FAILED(rv)) return rv;
    rv = o->Remove(PR_FALSE);
    if (NS_FAILED(rv)) return rv;

    /* Open file */
    if (!(outfile = fopen(path.get(), "w+"))) {
        fprintf(stderr, "Could not open OGG file\n");
        return NS_ERROR_FAILURE;
    }
    EscapeBackslash(path);
    file.Assign(path.get(), strlen(path.get()));
    
    if (ogg_stream_init(ogg_state, rand())) {
        fprintf(stderr, "Failed ogg_stream_init!\n");
        return NS_ERROR_FAILURE;
    }
    
    th_info_init(&ti);
    /* Must be multiples of 16 */
    ti.frame_width = ((WIDTH + 15) >> 4) << 4;
    ti.frame_height = ((HEIGHT + 15) >> 4) << 4;
    ti.pic_width = WIDTH;
    ti.pic_height = HEIGHT;
    ti.pic_x = 0;
    ti.pic_y = 0;
    
    /* Too fast? Why? */
    ti.fps_numerator = FPS_N;
    ti.fps_denominator = FPS_D;
    ti.aspect_numerator = 0;
    ti.aspect_denominator = 0;
    ti.colorspace = TH_CS_UNSPECIFIED;
    ti.pixel_fmt = TH_PF_420;
    ti.target_bitrate = 0;
    ti.quality = 48;
    
    encoder = th_encode_alloc(&ti);
    th_info_clear(&ti);
    
    /* Header init */
    th_comment_init(&tc);
    if (th_encode_flushheader(encoder, &tc, &packet) <= 0) {
        fprintf(stderr,"Internal Theora library error.\n");
        return NS_ERROR_FAILURE;
    }
    th_comment_clear(&tc);
    
    ogg_stream_packetin(ogg_state, &packet);
    if (ogg_stream_pageout(ogg_state, &page) != 1) {
        fprintf(stderr,"Internal Ogg library error.\n");
        return NS_ERROR_FAILURE;
    }
    fwrite(page.header, 1, page.header_len, outfile);
    fwrite(page.body, 1, page.body_len, outfile);
    
    /* Create remaining headers */
    for (;;) {
        ret = th_encode_flushheader(encoder, &tc, &packet);
        if (ret < 0){
            fprintf(stderr,"Internal Theora library error.\n");
            return NS_ERROR_FAILURE;
        } else if (!ret) break;
        ogg_stream_packetin(ogg_state, &packet);
    }
    
    /* Flush the rest of our headers. This ensures the actual data in each 
       stream will start on a new page, as per spec. */
    for (;;) {
        ret = ogg_stream_flush(ogg_state, &page);
        if (ret < 0){
            fprintf(stderr,"Internal Ogg library error.\n");
            return NS_ERROR_FAILURE;
        }
        if (ret == 0) break;
        fwrite(page.header, 1, page.header_len, outfile);
        fwrite(page.body, 1, page.body_len, outfile);
    }
    
    return NS_OK;
}

/*
 * Start recording to file
 */
NS_IMETHODIMP
VideoRecorder::Start(
    nsIDOMCanvasRenderingContext2D *ctx,
    nsACString &file
)
{
    nsresult rv;
    if (recording) {
        fprintf(stderr, "Recording in progress!\n");
        return NS_ERROR_FAILURE;
    }
    
    rv = SetupOggTheora(file);
    if (NS_FAILED(rv)) return rv;

    /* Acquire camera */
    if (!(source = vidcap_src_acquire(sapi, &sources[0]))) {
        fprintf(stderr, "Failed vidcap_src_acquire()\n");
        return NS_ERROR_FAILURE;
    }
    
    /* Acquire surface for callback */
    if (ctx) {
        mCtx = ctx;
    }
    
    /* Start recording */
    struct vidcap_fmt_info fmt_info;
    fmt_info.width = WIDTH;
    fmt_info.height = HEIGHT;
    fmt_info.fourcc = VIDCAP_FOURCC_I420;
    fmt_info.fps_numerator = FPS_N;
    fmt_info.fps_denominator = FPS_D;
    
    if (vidcap_format_bind(source, &fmt_info)) {
        fprintf(stderr, "Failed vidcap_format_bind()\n");
        return NS_ERROR_FAILURE;
    }
    
    /* Capture then start encode routine on new thread */
    if (vidcap_src_capture_start(source, VideoRecorder::Callback, this)) {
        fprintf(stderr, "Failed vidcap_src_capture_start()\n");
        return NS_ERROR_FAILURE;
    }
    
    encthr = PR_CreateThread(
        PR_SYSTEM_THREAD,
        VideoRecorder::Encode, this,
        PR_PRIORITY_NORMAL,
        PR_GLOBAL_THREAD,
        PR_JOINABLE_THREAD, 0
    );

    recording = 1;
    return NS_OK;
}

/*
 * Stop recording
 */
NS_IMETHODIMP
VideoRecorder::Stop()
{
    ogg_page page;
    
    if (!recording) {
        fprintf(stderr, "No recording in progress!\n");
        return NS_ERROR_FAILURE;    
    }
    if (vidcap_src_capture_stop(source)) {
        fprintf(stderr, "Failed vidcap_src_capture_stop()\n");
        return NS_ERROR_FAILURE;
    }
    vidcap_src_release(source);
    
    /* Wait for encoder to finish */
    mPipeOut->Close();
    PR_JoinThread(encthr);
    mPipeIn->Close();

    th_encode_free(encoder);
    if (ogg_stream_flush(ogg_state, &page)) {
        fwrite(page.header, page.header_len, 1, outfile);
        fwrite(page.body, page.body_len, 1, outfile);
    }
    fclose(outfile);
    ogg_stream_clear(ogg_state);
    
    recording = 0;
    return NS_OK;
}
