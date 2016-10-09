/*
    ALAC plugin for deadbeef
    Copyright (C) 2012-2013 Alexey Yakovenko <waker@users.sourceforge.net>
    Uses the reverse engineered ALAC decoder (C) 2005 David Hammerton
    All rights reserved.

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use,
    copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
    OTHER DEALINGS IN THE SOFTWARE.
 */
#include "../../deadbeef.h"
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define USE_APPLE_CODEC 1
#define USE_MP4FF 0

#if USE_APPLE_CODEC
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#endif

#include "mp4ff.h"
extern "C" {
#include "../../shared/mp4tagutil.h"
}

#if !USE_MP4FF
extern "C" {
#include "demux.h"
#include "stream.h"
#include "decomp.h"
}
#endif

#include "alac_plugin.h"

#define min(x,y) ((x)<(y)?(x):(y))
#define max(x,y) ((x)>(y)?(x):(y))

#ifndef __linux__
#define off64_t off_t
#define lseek64 lseek
#define O_LARGEFILE 0
#endif

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

extern DB_decoder_t alac_plugin;
extern DB_functions_t *deadbeef;

#if USE_MP4FF
#define MP4FILE mp4ff_t *
#define MP4FILE_CB mp4ff_callback_t
#endif

#ifdef WORDS_BIGENDIAN
int host_bigendian = 1;
#else
int host_bigendian = 0;
#endif

#define BUFFER_SIZE (1024*24)
#define IN_BUFFER_SIZE (1024*80)

typedef struct {
    DB_fileinfo_t info;
    DB_FILE *file;
#if USE_MP4FF
    MP4FILE mp4;
    MP4FILE_CB mp4reader;
    int mp4track;
    int mp4framesize;
    int mp4samples;
    int mp4sample;
#else
    demux_res_t demux_res;
    stream_t *stream;
    int64_t dataoffs;
    int current_frame;
#endif
#if USE_APPLE_CODEC
    ALACDecoder *alac;
#else
    alac_file *_alac;
#endif
    int junk;
    BitBuffer theInputBuffer;
    uint8_t buffer[IN_BUFFER_SIZE];
    uint8_t out_buffer[BUFFER_SIZE];
    int out_remaining;
    int skipsamples;
    int currentsample;
    int startsample;
    int endsample;
} alacplug_info_t;

// allocate codec control structure
DB_fileinfo_t *
alacplug_open (uint32_t hints) {
    DB_fileinfo_t *_info = (DB_fileinfo_t *)malloc (sizeof (alacplug_info_t));
    alacplug_info_t *info = (alacplug_info_t *)_info;
    memset (info, 0, sizeof (alacplug_info_t));
    return _info;
}

#if USE_MP4FF
static uint32_t
mp4_fs_read (void *user_data, void *buffer, uint32_t length) {
    alacplug_info_t *info = (alacplug_info_t *)user_data;
    return (uint32_t)deadbeef->fread (buffer, 1, length, info->file);
}

static uint32_t
mp4_fs_seek (void *user_data, uint64_t position) {
    alacplug_info_t *info = (alacplug_info_t *)user_data;
    return deadbeef->fseek (info->file, position+info->junk, SEEK_SET);
}

static int
mp4_track_get_info (mp4ff_t *mp4, int track, float *duration, int *samplerate, int *channels, int *bps, int64_t *totalsamples, int *mp4framesize) {
    unsigned char*  buff = 0;
    unsigned int    buff_size = 0;
    int samples = 0;

    if (mp4ff_get_decoder_config(mp4, track, &buff, &buff_size)) {
        return -1;
    }
    if (!buff) {
        return -1;
    }
    ALACDecoder dec;
    int rc = dec.Init(buff, buff_size);
    free (buff);

    if(rc < 0) {
        trace ("alac: ALACDecoder.Init returned %d\n", rc);
        return -1;
    }

    *samplerate = dec.mConfig.sampleRate;
    *channels = dec.mConfig.numChannels;
    *bps = dec.mConfig.bitDepth;

    samples = mp4ff_num_samples(mp4, track);

    if (samples <= 0) {
        return -1;
    }

    int i_sample_count = samples;
    int i_sample;

    int64_t total_dur = 0;
    for( i_sample = 0; i_sample < i_sample_count; i_sample++ )
    {
        total_dur += mp4ff_get_sample_duration (mp4, track, i_sample);
    }
    if (totalsamples) {
        *totalsamples = total_dur * (*samplerate) / mp4ff_time_scale (mp4, track);
        *mp4framesize = (int)((*totalsamples) / i_sample_count);
    }
    *duration = total_dur / (float)mp4ff_time_scale (mp4, track);

    return 0;
}
#else
static int
get_sample_info(demux_res_t *demux_res, uint32_t samplenum,
                           uint32_t *sample_duration,
                           uint32_t *sample_byte_size)
{
    unsigned int duration_index_accum = 0;
    unsigned int duration_cur_index = 0;

    if (samplenum >= demux_res->num_sample_byte_sizes)
    {
        fprintf(stderr, "sample %i does not exist\n", samplenum);
        return 0;
    }

    if (!demux_res->num_time_to_samples)
    {
        fprintf(stderr, "no time to samples\n");
        return 0;
    }
    while ((demux_res->time_to_sample[duration_cur_index].sample_count + duration_index_accum)
            <= samplenum)
    {
        duration_index_accum += demux_res->time_to_sample[duration_cur_index].sample_count;
        duration_cur_index++;
        if (duration_cur_index >= demux_res->num_time_to_samples)
        {
            fprintf(stderr, "sample %i does not have a duration\n", samplenum);
            return 0;
        }
    }

    *sample_duration = demux_res->time_to_sample[duration_cur_index].sample_duration;
    *sample_byte_size = demux_res->sample_byte_size[samplenum];

    return 1;
}

static int64_t
alacplug_get_totalsamples (demux_res_t *demux_res) {
    int64_t totalsamples = 0;
    for (int i = 0; i < demux_res->num_sample_byte_sizes; i++)
    {
        unsigned int thissample_duration = 0;
        unsigned int thissample_bytesize = 0;

        get_sample_info(demux_res, i, &thissample_duration,
                &thissample_bytesize);

        totalsamples += thissample_duration;
    }
    return totalsamples;
}
#endif

int
alacplug_init (DB_fileinfo_t *_info, DB_playItem_t *it) {
    alacplug_info_t *info = (alacplug_info_t *)_info;

    deadbeef->pl_lock ();
    info->file = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    if (!info->file) {
        return -1;
    }

    int64_t totalsamples = 0;

#if USE_MP4FF
    info->mp4track = -1;
    info->mp4reader.read = mp4_fs_read;
    info->mp4reader.write = NULL;
    info->mp4reader.seek = mp4_fs_seek;
    info->mp4reader.truncate = NULL;
    info->mp4reader.user_data = info;

    int samplerate = 0;
    int channels = 0;
    int bps;
    float duration = 0;

    trace ("alac_init: mp4ff_open_read %s\n", deadbeef->pl_find_meta (it, ":URI"));
    info->mp4 = mp4ff_open_read (&info->mp4reader);
    if (info->mp4) {
        int ntracks = mp4ff_total_tracks (info->mp4);
        for (int i = 0; i < ntracks; i++) {
            if (mp4ff_get_track_type (info->mp4, i) != TRACK_AUDIO) {
                continue;
            }
            int res = mp4_track_get_info (info->mp4, i, &duration, &samplerate, &channels, &bps, &totalsamples, &info->mp4framesize);
            if (res >= 0 && duration > 0) {
                info->mp4track = i;
                break;
            }
        }
        trace ("track: %d\n", info->mp4track);
        if (info->mp4track >= 0) {
            // prepare decoder
            int res = mp4_track_get_info (info->mp4, info->mp4track, &duration, &samplerate, &channels, &bps, &totalsamples, &info->mp4framesize);
            if (res != 0) {
                trace ("alac: mp4_track_get_info(%d) returned error\n", info->mp4track);
                return -1;
            }

            // init mp4 decoding
            info->mp4samples = mp4ff_num_samples(info->mp4, info->mp4track);
            info->alac = new ALACDecoder;
            unsigned char*  buff = 0;
            unsigned int    buff_size = 0;
            if (mp4ff_get_decoder_config (info->mp4, info->mp4track, &buff, &buff_size)) {
                return -1;
            }
            res = info->alac->Init(buff, buff_size);
            free (buff);

            if (res < 0) {
                trace ("ALACDecoder.Init returned %d\n", res);
                return -1;
            }

            trace ("alac: successfully initialized track %d\n", info->mp4track);
            _info->fmt.samplerate = samplerate;
            _info->fmt.channels = channels;
            _info->fmt.bps = bps;
        }
        else {
            trace ("alac: track not found in mp4 container\n");
            mp4ff_close (info->mp4);
            info->mp4 = NULL;
        }
    }
#else
    info->stream = stream_create_file (info->file, 1, info->junk);

    if (!qtmovie_read(info->stream, &info->demux_res)) {
        if (!info->demux_res.format_read || info->demux_res.format != MAKEFOURCC('a','l','a','c')) {
            return -1;
        }
    }
    info->dataoffs = deadbeef->ftell (info->file);

#if USE_APPLE_CODEC
    info->alac = new ALACDecoder;
    if (info->alac->Init(info->demux_res.codecdata+12, info->demux_res.codecdata_len-12) < 0) {
        return -1;
    }
    info->demux_res.sample_rate = info->alac->mConfig.sampleRate;
    info->demux_res.sample_size = info->alac->mConfig.bitDepth;
#else
    info->_alac = create_alac (info->demux_res.sample_size, info->demux_res.num_channels);
    alac_set_info(info->_alac, info->demux_res.codecdata);

    info->demux_res.sample_rate = alac_get_samplerate (info->_alac);
    info->demux_res.sample_size = alac_get_bitspersample (info->_alac);
#endif

    totalsamples = alacplug_get_totalsamples (&info->demux_res);
    _info->fmt.bps = info->demux_res.sample_size;
    _info->fmt.channels = info->demux_res.num_channels;
    _info->fmt.samplerate = info->demux_res.sample_rate;
#endif

    if (!info->file->vfs->is_streaming ()) {
        if (it->endsample > 0) {
            info->startsample = it->startsample;
            info->endsample = it->endsample;
            alacplug_seek_sample (_info, 0);
        }
        else {
            info->startsample = 0;
            info->endsample = (int)(totalsamples-1);
        }
    }

    _info->plugin = &alac_plugin;
    for (int i = 0; i < _info->fmt.channels; i++) {
        _info->fmt.channelmask |= 1 << i;
    }

    return 0;
}

void
alacplug_free (DB_fileinfo_t *_info) {
    alacplug_info_t *info = (alacplug_info_t *)_info;
    if (info) {
        if (info->file) {
            deadbeef->fclose (info->file);
        }
#if USE_MP4FF
        if (info->mp4) {
            mp4ff_close (info->mp4);
        }
#else
        if (info->stream) {
            stream_destroy (info->stream);
        }
        qtmovie_free_demux (&info->demux_res);
#endif
#if USE_APPLE_CODEC
        delete info->alac;
#else
        if (info->_alac) {
            alac_file_free (info->_alac);
        }
#endif

        free (info);
    }
}

int
alacplug_read (DB_fileinfo_t *_info, char *bytes, int size) {
    alacplug_info_t *info = (alacplug_info_t *)_info;
    int samplesize = _info->fmt.channels * _info->fmt.bps / 8;
    if (!info->file->vfs->is_streaming ()) {
        if (info->currentsample + size / samplesize > info->endsample) {
            size = (info->endsample - info->currentsample + 1) * samplesize;
            if (size <= 0) {
                trace ("alacplug_read: eof (current=%d, total=%d)\n", info->currentsample, info->endsample);
                return 0;
            }
        }
    }
    int initsize = size;
    while (size > 0) {
        // handle seeking
        if (info->skipsamples > 0 && info->out_remaining > 0) {
            int skip = min (info->out_remaining, info->skipsamples);
            if (skip < info->out_remaining) {
                memmove (info->out_buffer, info->out_buffer + skip * samplesize, (info->out_remaining - skip) * samplesize);
            }
            info->out_remaining -= skip;
            info->skipsamples -= skip;
        }
        if (info->out_remaining > 0) {
            int n = size / samplesize;
            n = min (info->out_remaining, n);

            uint8_t *src = info->out_buffer;
            memcpy (bytes, src, n * samplesize);
            bytes += n * samplesize;
            src += n * samplesize;
            size -= n * samplesize;

            if (n == info->out_remaining) {
                info->out_remaining = 0;
            }
            else {
                memmove (info->out_buffer, src, (info->out_remaining - n) * samplesize);
                info->out_remaining -= n;
            }
            continue;
        }

#if USE_MP4FF
        if (info->mp4sample >= info->mp4samples) {
            trace ("alac: finished with the last mp4sample\n");
            break;
        }

        unsigned char *buffer = NULL;
        uint32_t buffer_size = 0;
        int rc = mp4ff_read_sample (info->mp4, info->mp4track, info->mp4sample, &buffer, &buffer_size);
        if (rc == 0) {
            trace ("mp4ff_read_sample failed\n");
            break;
        }
        info->mp4sample++;

        BitBufferInit(&info->theInputBuffer, buffer, buffer_size);

        uint32_t outNumSamples = 0;
        int32_t err = info->alac->Decode(&info->theInputBuffer, info->out_buffer, BUFFER_SIZE/samplesize, info->alac->mConfig.numChannels, &outNumSamples);
        if (err) {
            break;
        }

        info->out_remaining += outNumSamples;

        if (buffer) {
            free (buffer);
        }
#else
        // decode next frame
        if (info->current_frame == info->demux_res.num_sample_byte_sizes) {
            break; // end of file
        }

        uint32_t sample_duration;
        uint32_t sample_byte_size;
        int outputBytes;
        /* just get one sample for now */
        if (!get_sample_info(&info->demux_res, info->current_frame,
                             &sample_duration, &sample_byte_size))
        {
            fprintf(stderr, "alac: sample failed\n");
            break;
        }
        if (IN_BUFFER_SIZE < sample_byte_size)
        {
            fprintf(stderr, "alac: buffer too small! (is %i want %i)\n",
                    IN_BUFFER_SIZE,
                    sample_byte_size);
            break;
        }


        int32_t nb = stream_read (info->stream, sample_byte_size, info->buffer);
        if (stream_eof (info->stream)) {
            break;
        }

        outputBytes = BUFFER_SIZE;
#if USE_APPLE_CODEC
        BitBufferInit(&info->theInputBuffer, info->buffer, nb);

        uint32_t outNumSamples = 0;
        int32_t err = info->alac->Decode(&info->theInputBuffer, info->out_buffer, BUFFER_SIZE/samplesize, info->alac->mConfig.numChannels, &outNumSamples);

        info->out_remaining += outNumSamples;

        //        BitBufferReset(&info->theInputBuffer);
#else
        decode_frame(info->_alac, info->buffer, info->out_buffer, &outputBytes);
        info->out_remaining += outputBytes / samplesize;
#endif
        info->current_frame++;
#endif
    }

    info->currentsample += (initsize-size) / samplesize;
    return initsize-size;
}

int
alacplug_seek_sample (DB_fileinfo_t *_info, int sample) {
    alacplug_info_t *info = (alacplug_info_t *)_info;

    sample += info->startsample;

#if USE_MP4FF
    int totalsamples = 0;
    int i;
    int num_sample_byte_sizes = mp4ff_get_num_sample_byte_sizes (info->mp4, info->mp4track);
    int scale = _info->fmt.samplerate / mp4ff_time_scale (info->mp4, info->mp4track);
    for (i = 0; i < num_sample_byte_sizes; i++)
    {
        unsigned int thissample_duration = 0;
        unsigned int thissample_bytesize = 0;

        mp4ff_get_sample_info(info->mp4, info->mp4track, i, &thissample_duration,
                              &thissample_bytesize);

        if (totalsamples + thissample_duration > sample / scale) {
            info->skipsamples = sample - totalsamples * scale;
            break;
        }
        totalsamples += thissample_duration;
    }
    //        i = sample / info->mp4framesize;
    //        info->skipsamples = sample - info->mp4sample * info->mp4framesize;
    info->mp4sample = i;
    trace ("seek res: frame %d (old: %d*%d), skip %d\n", info->mp4sample, sample / info->mp4framesize, info->mp4framesize, info->skipsamples);
#else

    int totalsamples = 0;
    int64_t seekpos = 0;
    int i;
    for (i = 0; i < info->demux_res.num_sample_byte_sizes; i++)
    {
        unsigned int thissample_duration = 0;
        unsigned int thissample_bytesize = 0;

        get_sample_info(&info->demux_res, i, &thissample_duration,
                &thissample_bytesize);

        if (totalsamples + thissample_duration > sample) {
            info->skipsamples = sample - totalsamples;
            break;
        }
        totalsamples += thissample_duration;
        seekpos += info->demux_res.sample_byte_size[i];
    }

    if (i == info->demux_res.num_sample_byte_sizes) {
        return -1;
    }


    deadbeef->fseek(info->file, info->dataoffs + seekpos, SEEK_SET);

    info->current_frame = i;
#endif
    info->out_remaining = 0;
    info->currentsample = sample;
    _info->readpos = (float)(info->currentsample - info->startsample) / _info->fmt.samplerate;
    return 0;
}

int
alacplug_seek (DB_fileinfo_t *_info, float t) {
    return alacplug_seek_sample (_info, t * _info->fmt.samplerate);
}

DB_playItem_t *
alacplug_insert (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname) {
    trace ("adding %s\n", fname);

    int samplerate = 0;
    int bps = 0;
    int channels = 0;
#if !USE_MP4FF
    int64_t fsize = 0;
    ALACDecoder *alac = NULL;
#endif
    int64_t totalsamples = 0;

    alacplug_info_t info;
    mp4ff_t *mp4 = NULL;
    DB_playItem_t *it = NULL;

#if !USE_MP4FF
    demux_res_t demux_res;
    stream_t *stream;

    memset (&demux_res, 0, sizeof (demux_res));
#endif

    memset (&info, 0, sizeof (info));

    DB_FILE *fp = deadbeef->fopen (fname);
    if (!fp) {
        trace ("not found\n");
        return NULL;
    }

    info.file = fp;
    info.junk = deadbeef->junk_get_leading_size (fp);
    if (info.junk >= 0) {
        trace ("junk: %d\n", info.junk);
        deadbeef->fseek (fp, info.junk, SEEK_SET);
    }
    else {
        info.junk = 0;
    }

#if USE_MP4FF

    const char *ftype = NULL;
    float duration = -1;

    // slowwww!
    info.file = fp;
    MP4FILE_CB cb = {
        .read = mp4_fs_read,
        .write = NULL,
        .seek = mp4_fs_seek,
        .truncate = NULL,
        .user_data = &info
    };

    mp4 = mp4ff_open_read (&cb);
    if (mp4) {
        int ntracks = mp4ff_total_tracks (mp4);
        trace ("alac: numtracks=%d\n", ntracks);
        int i;
        for (i = 0; i < ntracks; i++) {
            if (mp4ff_get_track_type (mp4, i) != TRACK_AUDIO) {
                trace ("alac: track %d is not audio\n", i);
                continue;
            }
            int mp4framesize;
            int res = mp4_track_get_info (mp4, i, &duration, &samplerate, &channels, &bps, &totalsamples, &mp4framesize);
            if (res >= 0 && duration > 0) {
                trace ("alac: found audio track %d (duration=%f, totalsamples=%d)\n", i, duration, totalsamples);

#if 0 // TODO
                int num_chapters = 0;
                mp4_chapter_t *chapters = NULL;
                if (mp4ff_chap_get_num_tracks(mp4) > 0) {
                    chapters = mp4_load_itunes_chapters (mp4, &num_chapters, samplerate);
                }
#endif
                DB_playItem_t *it = deadbeef->pl_item_alloc_init (fname, alac_plugin.plugin.id);
                ftype = "ALAC";
                deadbeef->pl_add_meta (it, ":FILETYPE", ftype);
                deadbeef->pl_set_meta_int (it, ":TRACKNUM", i);
                deadbeef->plt_set_item_duration (plt, it, duration);

                deadbeef->rewind (fp);
                mp4_read_metadata_file(it, fp);

                int64_t fsize = deadbeef->fgetlength (fp);
                deadbeef->fclose (fp);

                char s[100];
                snprintf (s, sizeof (s), "%lld", fsize);
                deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
                deadbeef->pl_add_meta (it, ":BPS", "16");
                snprintf (s, sizeof (s), "%d", channels);
                deadbeef->pl_add_meta (it, ":CHANNELS", s);
                snprintf (s, sizeof (s), "%d", samplerate);
                deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
                int br = (int)roundf(fsize / duration * 8 / 1000);
                snprintf (s, sizeof (s), "%d", br);
                deadbeef->pl_add_meta (it, ":BITRATE", s);

#if 0 // TODO
                // embedded chapters
                deadbeef->pl_lock (); // FIXME: is it needed?
                if (chapters && num_chapters > 0) {
                    DB_playItem_t *cue = mp4_insert_with_chapters (plt, after, it, chapters, num_chapters, totalsamples, samplerate);
                    for (int n = 0; n < num_chapters; n++) {
                        if (chapters[n].title) {
                            free (chapters[n].title);
                        }
                    }
                    free (chapters);
                    if (cue) {
                        mp4ff_close (mp4);
                        deadbeef->pl_item_unref (it);
                        deadbeef->pl_item_unref (cue);
                        deadbeef->pl_unlock ();
                        return cue;
                    }
                }
#endif

                // embedded cue
                const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
                DB_playItem_t *cue = NULL;

                if (cuesheet) {
                    cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, (const uint8_t *)cuesheet, (int)strlen (cuesheet), (int)totalsamples, samplerate);
                    if (cue) {
                        mp4ff_close (mp4);
                        deadbeef->pl_item_unref (it);
                        deadbeef->pl_item_unref (cue);
                        deadbeef->pl_unlock ();
                        return cue;
                    }
                }
                deadbeef->pl_unlock ();

                cue  = deadbeef->plt_insert_cue (plt, after, it, (int)totalsamples, samplerate);
                if (cue) {
                    deadbeef->pl_item_unref (it);
                    deadbeef->pl_item_unref (cue);
                    return cue;
                }

                after = deadbeef->plt_insert_item (plt, after, it);
                deadbeef->pl_item_unref (it);
                break;
            }
        }
        mp4ff_close (mp4);
        if (i < ntracks) {
            return after;
        }
        // mp4 container found, but no valid alac tracks in it
        return NULL;
    }

#else

    float duration = -1;

    stream = stream_create_file (fp, 1, info.junk);
    if (!stream) {
        trace ("alac: stream_create_file failed\n");
        goto error;
    }

    if (!qtmovie_read(stream, &demux_res)) {
        if (!demux_res.format_read || demux_res.format != MAKEFOURCC('a','l','a','c')) {
            trace ("alac track not found in the file %s, expected atom %X got %X\n", fname, MAKEFOURCC('a','l','a','c'), demux_res.format);
            goto error;
        }
    }

    alac = new ALACDecoder;
    if (alac->Init(demux_res.codecdata+12, demux_res.codecdata_len-12) < 0) {
        delete alac;
        goto error;
    }
    demux_res.sample_rate = alac->mConfig.sampleRate;
    demux_res.sample_size = alac->mConfig.bitDepth;
    delete alac;

    it = deadbeef->pl_item_alloc_init (fname, alac_plugin.plugin.id);
    deadbeef->pl_add_meta (it, ":FILETYPE", "ALAC");

    totalsamples = alacplug_get_totalsamples (&demux_res);
    duration = totalsamples / (float)demux_res.sample_rate;

    deadbeef->plt_set_item_duration (plt, it, duration);

    deadbeef->rewind (fp);
    mp4_read_metadata_file (it, fp);

    fsize = deadbeef->fgetlength (fp);

    deadbeef->fclose (fp);
    fp = NULL;

    stream_destroy (stream);
    stream = NULL;
    if (mp4) {
        mp4ff_close (mp4);
        mp4 = NULL;
    }
    samplerate = demux_res.sample_rate;
    bps = demux_res.sample_size;
    channels = demux_res.num_channels;

    qtmovie_free_demux (&demux_res);

    trace ("duration %f\n", duration);
    if (duration > 0) {
        char s[100];
        snprintf (s, sizeof (s), "%lld", fsize);
        deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
        snprintf (s, sizeof (s), "%d", bps);
        deadbeef->pl_add_meta (it, ":BPS", s);
        snprintf (s, sizeof (s), "%d", channels);
        deadbeef->pl_add_meta (it, ":CHANNELS", s);
        snprintf (s, sizeof (s), "%d", samplerate);
        deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
        int br = (int)roundf(fsize / duration * 8 / 1000);
        snprintf (s, sizeof (s), "%d", br);
        deadbeef->pl_add_meta (it, ":BITRATE", s);
        // embedded cue
        deadbeef->pl_lock ();
        const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
        DB_playItem_t *cue = NULL;

        if (cuesheet) {
            cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, (const uint8_t *)cuesheet, (int)strlen (cuesheet), totalsamples, samplerate);
            if (cue) {
                deadbeef->pl_item_unref (it);
                deadbeef->pl_item_unref (cue);
                deadbeef->pl_unlock ();
                return cue;
            }
        }
        deadbeef->pl_unlock ();

        cue  = deadbeef->plt_insert_cue (plt, after, it, totalsamples, samplerate);
        if (cue) {
            deadbeef->pl_item_unref (it);
            deadbeef->pl_item_unref (cue);
            return cue;
        }
    }

    trace ("success\n");
success:
    after = deadbeef->plt_insert_item (plt, after, it);
    deadbeef->pl_item_unref (it);
error:
    if (fp) {
        deadbeef->fclose (fp);
    }
    if (mp4) {
        mp4ff_close (mp4);
    }
    qtmovie_free_demux (&demux_res);
#endif
    return it;
}
