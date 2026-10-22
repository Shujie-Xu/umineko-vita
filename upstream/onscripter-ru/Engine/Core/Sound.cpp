/**
 *  Sound.cpp
 *  ONScripter-RU
 *
 *  Methods for playing sound.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Components/Window.hpp"
#include "Engine/Readers/Base.hpp"
#include "Support/FileIO.hpp"

#ifdef LINUX
#include <signal.h>
#endif

#if defined(INTEGER_OGG_VORBIS)
#include <tremor/ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif

#include <cstdio>
#ifdef VITA
#include <cstring>
#include <malloc.h>
#include <strings.h>
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#include "platform/vita/memory.hpp"
#endif

struct WAVE_HEADER {
	char chunk_riff[4];
	char riff_length[4];
	// format chunk
	char fmt_id[8];
	char fmt_size[4];
	char data_fmt[2];
	char channels[2];
	char frequency[4];
	char byte_size[4];
	char sample_byte_size[2];
	char sample_bit_size[2];
};

struct WAVE_DATA_HEADER {
	// data chunk
	char chunk_id[4];
	char data_length[4];
};

static void setupWaveHeader(uint8_t *buffer, int channels, int bits,
                            unsigned long rate, size_t data_length,
                            size_t extra_bytes = 0, uint8_t *extra_ptr = nullptr);

extern bool ext_music_play_once_flag;

extern "C" {
extern void musicFinishCallback();
}
extern void seqmusicCallback(int sig);

#define TMP_MUSIC_FILE "tmp.mus"

//AVI header format
#define IS_AVI_HDR(buf)                        \
	(((buf)[0] == 'R') && ((buf)[1] == 'I') && \
	 ((buf)[2] == 'F') && ((buf)[3] == 'F') && \
	 ((buf)[8] == 'A') && ((buf)[9] == 'V') && \
	 ((buf)[10] == 'I'))

//ID3v2 tag header format
#define HAS_ID3V2_TAG(buf)                                          \
	(((buf)[0] == 'I') && ((buf)[1] == 'D') && ((buf)[2] == '3') && \
	 ((buf)[3] != 0xFF) && ((buf)[4] != 0xFF) && !((buf)[5] & 0x1F))

struct OVInfo {
	uint8_t *buf;
	ogg_int64_t length;
	ogg_int64_t pos;
	OggVorbis_File ovf;
};

static size_t oc_read_func(void *ptr, size_t size, size_t nmemb, void *datasource) {
	OVInfo *ogg_vorbis_info = static_cast<OVInfo *>(datasource);

	size_t len = size * nmemb;
	if (static_cast<size_t>(ogg_vorbis_info->pos) + len > static_cast<size_t>(ogg_vorbis_info->length))
		len = static_cast<size_t>(ogg_vorbis_info->length - ogg_vorbis_info->pos);
	std::memcpy(ptr, ogg_vorbis_info->buf + ogg_vorbis_info->pos, len);
	ogg_vorbis_info->pos += len;

	return len;
}

static int oc_seek_func(void *datasource, ogg_int64_t offset, int whence) {
	OVInfo *ogg_vorbis_info = static_cast<OVInfo *>(datasource);

	ogg_int64_t pos = 0;
	if (whence == 0)
		pos = offset;
	else if (whence == 1)
		pos = ogg_vorbis_info->pos + offset;
	else if (whence == 2)
		pos = ogg_vorbis_info->length + offset;

	if (pos < 0 || pos > ogg_vorbis_info->length)
		return -1;

	ogg_vorbis_info->pos = pos;

	return 0;
}

static long oc_tell_func(void *datasource) {
	OVInfo *ogg_vorbis_info = static_cast<OVInfo *>(datasource);

	return static_cast<long>(ogg_vorbis_info->pos);
}

void ONScripter::loadSoundIntoCache(int id, const std::string &filename_str, bool async) {

	int ret{0};
	if (async) {
		ret = playSound(filename_str.c_str(), SOUND_PRELOAD | SOUND_CHUNK, false, MIX_CACHE_CHANNEL_ASYNC);
	} else {
		ret = playSoundThreaded(filename_str.c_str(), SOUND_PRELOAD | SOUND_CHUNK, false, MIX_CACHE_CHANNEL_BLOCK);
	}

	if (ret == SOUND_NONE) {
		if (pending_cache_chunk[async])
			pending_cache_chunk[async] = nullptr;
		sendToLog(LogLevel::Error, "Failed to cache sound %s in slot %d with async %d\n", filename_str.c_str(), id, async);
		return;
	}

	assert(pending_cache_chunk[async]);
	{
		Lock lock(&soundCache);
		soundCache.add(id, filename_str, pending_cache_chunk[async]);
	}
	pending_cache_chunk[async] = nullptr;

	//sendToLog(LogLevel::Info, "Cached sound %s in slot %d with async %d\n", filename_str.c_str(), id, async);
}

int ONScripter::trySoundCache(const char *filename, int format, bool loop_flag, int channel) {
	if (format & SOUND_CHUNK) {
		std::shared_ptr<Wrapped_Mix_Chunk> r{nullptr};
		{
			Lock lock(&soundCache);
			r = soundCache.get(filename);
#ifdef VITA
			if (r && channel != MIX_CACHE_CHANNEL_ASYNC && channel != MIX_CACHE_CHANNEL_BLOCK)
				nativeConsumePrefetchedLoop(filename);
#endif
		}
		// I hope shared_ptr is thread safe...
		if (r && r->chunk) {
			if (channel == MIX_CACHE_CHANNEL_ASYNC || channel == MIX_CACHE_CHANNEL_BLOCK) {
				bool async{channel == MIX_CACHE_CHANNEL_ASYNC};
				assert(!pending_cache_chunk[async]);
				pending_cache_chunk[async] = r;
			} else if (playWave(r, format, loop_flag, channel) != 0) {
				errorAndExit("Something mad was found in sound cache");
			}
			return SOUND_CHUNK;
		}
	}
	return SOUND_NONE;
}

int ONScripter::playSoundThreaded(const char *filename, int format, bool loop_flag, int channel, bool waitevent) {
	// note that this function is blocking -- it will not return until playSound is done
	// this function should be used instead of playSound pretty much all of the time,
	// to run playSound in a thread so that our main loop can continue without being horribly stalled

	//stop lipsEvent from being called, otherwise we will get lips broken due to loadLips calls in playSound -> playWave
	skipLipsAction = true;
#ifdef VITA
	// A script load in flight: lookahead prefetch reads pause meanwhile.
	struct ScriptLoad {
		ONScripter &ons;
		explicit ScriptLoad(ONScripter &o) : ons(o) { ons.nativeSoundLoads++; }
		~ScriptLoad() {
			ons.nativeScriptLoadEnd = SDL_GetTicks();
			ons.nativeSoundLoads--;
		}
	} scriptLoad(*this);
	nativeAwaitPrefetchedLoop(filename, waitevent);
#endif

	int cacheRet = trySoundCache(filename, format, loop_flag, channel);
	if (cacheRet) {
		skipLipsAction = false;
		return cacheRet;
	}
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	const auto waitStart = SDL_GetTicks();
#endif

	// set up the normal playSound call
	async.playSound(filename, format, loop_flag, channel);

	preventExit(true);
	while (SDL_SemWaitTimeout(async.playSoundQueue.resultsWaiting, 1) != 0) {
		if (waitevent) {
			event_mode = IDLE_EVENT_MODE;
			Lock lock(&playSoundThreadedLock);
			waitEvent(0);
		}
	}
	preventExit(false);

	// pop the result of playSound and return it
	SDL_AtomicLock(&async.playSoundQueue.resultsLock);
	auto r = reinterpret_cast<uintptr_t>(async.playSoundQueue.results.front());
	async.playSoundQueue.results.pop_front();
	SDL_AtomicUnlock(&async.playSoundQueue.resultsLock);
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	// Diagnostic builds: how long the script stood still for this sound.
	const auto waited = SDL_GetTicks() - waitStart;
	if (waited >= 5)
		sendToLog(LogLevel::Warn, "Timing %u: sound %s (channel %d): script waited %u ms\n",
		          static_cast<unsigned>(SDL_GetTicks()), filename, channel,
		          static_cast<unsigned>(waited));
#endif

	//enable lipsEvent
	skipLipsAction = false;

	return static_cast<int>(r);
}

int ONScripter::playSound(const char *filename, int format, bool loop_flag, int channel) {
	// This function modifies the following global variables:
	// music_info, script_h.errbuf, music_buffer, music_buffer_length, wave_sample, kv.second.lipsData.speechStart, audio_format, audio_open_flag

	if (!audio_open_flag)
		return SOUND_NONE;

	//Mion: account for mode_wave_demo setting
	//(i.e. if not set, then don't play non-bgm wave/ogg during skip mode)
	if (!mode_wave_demo_flag &&
	    // --- ^ --------------------------------------
	    // r.o. except for define section, should be ok
	    // --------------------------------------------
	    ((skip_mode & SKIP_NORMAL) || keyState.ctrl)) {
		// --- ^ ------------------------- ^ ---------------------------------------------------------
		// these globals are unmutexed, which is not ideal, but we think there will be no major issues
		// -------------------------------------------------------------------------------------------
		if ((format & SOUND_CHUNK) &&
		    ((channel < ONS_MIX_CHANNELS) || (channel == MIX_WAVE_CHANNEL)))
			return SOUND_NONE;
	}

#ifdef VITA
	// The script sets mode_wave_demo, so every skipped line decodes its whole
	// voice Ogg (Vorbis + resample to 48 kHz stereo) on the critical path, only
	// for the next line to stop it. While skipping, load a silent chunk for
	// voice files instead: dwaveplay still finds the channel loaded, and
	// looping MEs/SEs (outside sound/voice) keep their real data.
	if (((skip_mode & SKIP_NORMAL) || keyState.ctrl) && (format & SOUND_CHUNK) &&
	    channel < ONS_MIX_CHANNELS && std::strlen(filename) > 12 &&
	    !strncasecmp(filename, "sound", 5) && !strncasecmp(filename + 6, "voice", 5) &&
	    (filename[5] == '\\' || filename[5] == '/') && (filename[11] == '\\' || filename[11] == '/')) {
		// Whole frames for any mixer format, and enough for loadLips' first
		// sample read; QuickLoad chunks never free this buffer.
		alignas(8) static Uint8 silence[16]{};
		Mix_Chunk *chunk = Mix_QuickLoad_RAW(silence, sizeof(silence));
		if (chunk && playWave(std::make_shared<Wrapped_Mix_Chunk>(chunk), format, loop_flag, channel) == 0)
			return SOUND_CHUNK;
	}
#endif

	int cacheRet = trySoundCache(filename, format, loop_flag, channel);
	if (cacheRet)
		return cacheRet;

	size_t length{0};
	uint8_t *buffer{nullptr};
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	auto timingStart = SDL_GetTicks();
#endif
	{
		Lock lock(&music_file_name);
		// ------------- ^ ---------------------------------------------------------------------------
		// ! locked using a different lock to image, make sure all readers can access separate files !
		// at this moment only DirectReader is reliable
		// -------------------------------------------------------------------------------------------
		if (!script_h.reader->getFile(filename, length, &buffer))
			return SOUND_NONE;
	}
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	// Diagnostic builds: the sound thread's read and decode time per file.
	const auto readMs = SDL_GetTicks() - timingStart;
	timingStart += readMs;
	auto logSoundTiming = [&](const char *stage, unsigned decodedKiB) {
		const auto stageMs = SDL_GetTicks() - timingStart;
		if (readMs + stageMs >= 5)
			sendToLog(LogLevel::Warn, "Timing %u: sound %s (channel %d): read %u ms (%u KiB), %s %u ms (%u KiB)\n",
			          static_cast<unsigned>(SDL_GetTicks()), filename,
			          channel, static_cast<unsigned>(readMs), static_cast<unsigned>(length / 1024), stage,
			          static_cast<unsigned>(stageMs), decodedKiB);
	};
#endif

	if (length == 0)
		return SOUND_NONE;

	if ((channel == MIX_CACHE_CHANNEL_BLOCK || channel == MIX_CACHE_CHANNEL_ASYNC) &&
	    !(format & SOUND_CHUNK && format & SOUND_PRELOAD)) {
		freearr(&buffer);
		errorAndExit("Invalid sound cache call");
		return SOUND_NONE; //dummy
	}

	if ((format & SOUND_CHUNK) && !buffer[0] && !buffer[1] && !buffer[2] && !buffer[3]) {
		// "chunk" sound files would have a 4+ byte magic number,
		// so this could be a WAV with a bad (encrypted?) header;
		// will recreate the header from a ".fmt" file if one exists
		// assumes the first 128 bytes are bad (encrypted)
		// _and_ that the file contains uncompressed PCM data
		char *fmtname = new char[std::strlen(filename) + std::strlen(".fmt") + 1];
		std::sprintf(fmtname, "%s.fmt", filename);

		size_t fmtlen{0};
		uint8_t *fmtbuffer{nullptr};
		{
			Lock lock(&music_file_name);
			script_h.reader->getFile(fmtname, fmtlen, &fmtbuffer);
		}

		if (fmtlen >= 8) {
			// a file called filename + ".fmt" exists, of appropriate size;
			// read fmt info
			int channels, bits;
			unsigned long rate = 0, data_length = 0;

			channels = fmtbuffer[0];
			for (int i = 5; i > 1; i--) {
				rate = (rate << 8) + fmtbuffer[i];
			}
			bits = fmtbuffer[6];
			if (fmtlen >= 12) {
				// read the data_length
				for (int i = 11; i > 7; i--) {
					data_length = (data_length << 8) + fmtbuffer[i];
				}
			} else {
				// no data_length provided, fake it from the buffer length
				data_length = length - sizeof(WAVE_HEADER) - sizeof(WAVE_DATA_HEADER);
			}
			uint8_t fill = 0;
			if (bits == 8)
				fill = 128;
			for (size_t i = 0; (i < 128 && i < length); i++) {
				//clear the first 128 bytes (encryption noise)
				buffer[i] = fill;
			}
			if (fmtlen > 12) {
				setupWaveHeader(buffer, channels, bits, rate, data_length, fmtlen - 12, fmtbuffer + 12);
			} else {
				setupWaveHeader(buffer, channels, bits, rate, data_length);
			}
			// -------- ^^ ---------------------------------------------
			// local vars only, should be ok
			// ---------------------------------------------------------
			if ((bits == 8) && (fmtlen < 12)) {
				//hack: clear likely "pad bytes" at the end of the buffer
				//      (only on 8-bit samples when the fmt file doesn't
				//      include the data length)
				int i = 1;
				while (i < 5 && buffer[length - i] == 0) {
					buffer[length - i] = fill;
					i++;
				}
			}
		}
		freearr(&fmtbuffer);
		freearr(&fmtname);
	}

	if (format & SOUND_MUSIC) {
		int id3v2_size = 0;
		if (HAS_ID3V2_TAG(buffer)) {
			//found an ID3v2 tag, skipping since SMPEG doesn't
			for (int i = 0; i < 4; i++) {
				if (buffer[6 + i] & 0x80) { //err music_buffer brb, sorry {think of it}. See above ^
					id3v2_size = 0;
					break;
				}
				id3v2_size <<= 7;
				id3v2_size += buffer[6 + i];
			}
			if (id3v2_size > 0) {
				id3v2_size += 10;
				sendToLog(LogLevel::Info, "found ID3v2 tag in file '%s', size %d bytes\n", filename, id3v2_size);
			}
		}
		uint8_t *m_buf   = buffer + id3v2_size;
		const long m_len = length - id3v2_size;

		Mix_Music *music_info_local = Mix_LoadMUS_RW(SDL_RWFromMem(m_buf, static_cast<int>(m_len)), 0);
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
		logSoundTiming("stream open", 0);
#endif

		if (music_info_local) {
			if (match_bgm_audio_flag) {
				// -------- ^ --------------------------------------------
				// r.o. except for parseOptions, should be fine
				// -------------------------------------------------------
				//check how well the music matches the current mixer spec
				Mix_MusicType mtype  = Mix_GetMusicType(music_info_local);
				SDL_AudioSpec wanted = audio_format;
				bool change_spec     = false;

				if (mtype == MUS_MP3) {
#ifndef VITA
					SMPEG *mp3_chk = SMPEG_new_rwops(SDL_RWFromMem(m_buf, static_cast<int>(m_len)), nullptr, 0, 0);
					SMPEG_wantedSpec(mp3_chk, &wanted);
					SMPEG_delete(mp3_chk);
					if ((wanted.freq != audio_format.freq) ||
					    (wanted.format != audio_format.format))
						change_spec = true;
#endif
				}

				if (mtype == MUS_OGG) {
					OVInfo *ovi = new OVInfo();
					ovi->buf    = m_buf;
					ovi->length = m_len;
					ovi->pos    = 0;
					//annoying having to set callbacks just to check the specs...
					ov_callbacks oc;
					oc.read_func  = oc_read_func;
					oc.seek_func  = oc_seek_func;
					oc.close_func = nullptr;
					oc.tell_func  = oc_tell_func;
					if (ov_test_callbacks(ovi, &ovi->ovf, nullptr, 0, oc) >= 0) {
						vorbis_info *vi = ov_info(&ovi->ovf, -1);
						if (vi) {
							wanted.channels = vi->channels;
							wanted.freq     = static_cast<int>(vi->rate);
							wanted.format   = AUDIO_S16;
						}
						ov_clear(&ovi->ovf);
					}
					delete ovi;
				}

				if (mtype == MUS_WAV) {
					WAVE_HEADER *wav_hdr = reinterpret_cast<WAVE_HEADER *>(m_buf);
					wanted.freq          = wav_hdr->frequency[3];
					wanted.freq          = (wanted.freq << 8) + wav_hdr->frequency[2];
					wanted.freq          = (wanted.freq << 8) + wav_hdr->frequency[1];
					wanted.freq          = (wanted.freq << 8) + wav_hdr->frequency[0];
				}

				if (!change_spec && (wanted.freq != audio_format.freq)) {
					change_spec = true;
					//don't change for an ogg/wav w/frequency factor 2 or 4,
					//since SDL can convert it fine
					if ((wanted.freq * 2 == audio_format.freq) ||
					    (wanted.freq * 4 == audio_format.freq) ||
					    (audio_format.freq * 2 == wanted.freq) ||
					    (audio_format.freq * 4 == wanted.freq))
						change_spec = false;
				}
				if (change_spec) {
					//audio spec doesn't match well enough, reset the mixer
					//(and also free & reload the music_info)
					Mix_FreeMusic(music_info_local);
					Mix_CloseAudio();
					// resetting the mixer will stop all current sounds,
					// and a new spec can mess with preloaded chunks --
					// need to check for preloads, and either free them
					// or change their audiocvt settings FIXME
					//(at least until we get some decent audioconverters...)
					openAudio(wanted);
					if (!audio_open_flag) {
						// didn't work, use the old settings
						openAudio(default_audio_format);
					}
					// ^ openAudio() writes to globals, and audio_open_flag is also global, but, nothing in event uses either
					// and we won't be processing commands or anything as long as we are in this method

					// -----------------------------------------------------------------------------------
					// !!!! ^^^^^^ old problematic comment spotted ^^^^^ !!!!
					// -----------------------------------------------------------------------------------

					music_info_local = Mix_LoadMUS_RW(SDL_RWFromMem(buffer + id3v2_size, static_cast<int>(length - id3v2_size)), 0);
				}
			}

			setMusicVolume(music_volume, volume_on_flag);
			Mix_HookMusicFinished(musicFinishCallback);
			if (Mix_PlayMusic(music_info_local, music_play_loop_flag ? -1 : 0) == 0) {
				Lock lock(&playSoundThreadedLock);
				assert(!music_buffer);
				music_info          = music_info_local;
				music_buffer        = buffer;
				music_buffer_length = length;
				return SOUND_MUSIC;
			}
		} else {
			char errBuf[MAX_ERRBUF_LEN];
			std::snprintf(errBuf, MAX_ERRBUF_LEN, "error playing music '%s': %s\n", filename, Mix_GetError());
			freearr(&buffer);
			errorAndExit(errBuf);
			return SOUND_NONE; //dummy
		}
		Lock lock(&playSoundThreadedLock);
		music_info = music_info_local;
	}

	if (format & SOUND_CHUNK) {
		Mix_Chunk *chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, static_cast<int>(length)), 1);
#ifdef VITA
		const bool cacheOnly{channel == MIX_CACHE_CHANNEL_BLOCK || channel == MIX_CACHE_CHANNEL_ASYNC};
		if (!chunk && cacheOnly) {
			// Caching is optional; loadSoundIntoCache logs and carries on.
			sendToLog(LogLevel::Error, "Could not decode %s for the sound cache: %s\n", filename, Mix_GetError());
			freearr(&buffer);
			return SOUND_NONE;
		}
		if (!chunk) {
			// The whole Ogg is decoded to 48 kHz stereo and copied into one
			// contiguous block (5.9 MiB for a 32 s 22 kHz mono ME), which a full
			// or fragmented 192 MiB newlib heap may not have. Retry once
			// without the decoded-sound cache and the lookahead prefetches
			// before giving up.
			{
				Lock lock(&soundCache);
				soundCache.clearAll();
			}
			nativeReleasePrefetches();
			chunk = Mix_LoadWAV_RW(SDL_RWFromMem(buffer, static_cast<int>(length)), 1);
		}
#endif
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
		logSoundTiming("decode", chunk ? static_cast<unsigned>(chunk->alen / 1024) : 0);
#endif
		if (!chunk) {
			char errBuf[MAX_ERRBUF_LEN];
#ifdef VITA
			// Release builds have no log file: put the memory state in the
			// dialog so a screenshot tells heap exhaustion (little free) from
			// fragmentation, and shows whether full vitaGL pools pushed
			// textures into the newlib heap (vitaGL's use_extra_mem fallback).
			struct mallinfo mi = mallinfo();
			std::snprintf(errBuf, MAX_ERRBUF_LEN,
			              "error playing sound '%s': %s (heap used %lu KiB, free %lu KiB; GL free RAM %lu KiB, VRAM %lu KiB)\n",
			              filename, Mix_GetError(), static_cast<unsigned long>(mi.uordblks / 1024),
			              static_cast<unsigned long>(mi.fordblks / 1024),
			              static_cast<unsigned long>(vglMemFree(VGL_MEM_RAM) / 1024),
			              static_cast<unsigned long>(vglMemFree(VGL_MEM_VRAM) / 1024));
#else
			std::snprintf(errBuf, MAX_ERRBUF_LEN, "error playing sound '%s': %s\n", filename, Mix_GetError());
#endif
			freearr(&buffer);
			errorAndExit(errBuf);
			return SOUND_NONE; //dummy
		} else if (channel == MIX_CACHE_CHANNEL_BLOCK || channel == MIX_CACHE_CHANNEL_ASYNC) {
			// We are here to cache our Mix_Chunk and nothing else
			bool async{channel == MIX_CACHE_CHANNEL_ASYNC};
			assert(!pending_cache_chunk[async]);
			pending_cache_chunk[async] = std::make_shared<Wrapped_Mix_Chunk>(chunk);
			freearr(&buffer);
			return SOUND_CHUNK; //doesn't matter what to return
		} else {
			if (playWave(std::make_shared<Wrapped_Mix_Chunk>(chunk), format, loop_flag, channel) == 0) {
				freearr(&buffer);
				return SOUND_CHUNK;
			}
		}
	}

	if (format & SOUND_SEQMUSIC) {
		FILE *fp;
		{
			Lock lock(&music_file_name);
			fp = FileIO::openFile(TMP_MUSIC_FILE, "wb", script_h.getSavePath(TMP_MUSIC_FILE));
		}
		if (fp == nullptr) {
			char errBuf[MAX_ERRBUF_LEN];
			std::snprintf(errBuf, MAX_ERRBUF_LEN,
			              "can't open temporary music file %s", TMP_MUSIC_FILE);
			errorAndExit(errBuf);
		} else {
			{
				Lock lock(&music_file_name);
				if (std::fwrite(buffer, 1, length, fp) != length) {
					char errBuf[MAX_ERRBUF_LEN];
					std::snprintf(errBuf, MAX_ERRBUF_LEN,
					              "can't write to temporary music file %s",
					              TMP_MUSIC_FILE);
					errorAndExit(errBuf);
				}
				std::fclose(fp);
			}
			Lock lock(&playSoundThreadedLock);
			ext_music_play_once_flag = !loop_flag;
			if (playSequencedMusic(loop_flag) == 0) {
				freearr(&buffer);
				return SOUND_SEQMUSIC;
			}
		}
	}

	freearr(&buffer);
	return SOUND_OTHER;
}

void ONScripter::playCDAudio() {
	if (!audio_open_flag)
		return;

	//Search the "cd" subfolder
	//for a file named "track01.mp3" or similar, depending on the
	//track number; check for mp3, ogg and wav files
	char filename[256];
	std::sprintf(filename, R"(cd\track%2.2d.mp3)", current_cd_track);
	int ret = playSoundThreaded(filename, SOUND_MUSIC, cd_play_loop_flag);
	if (ret == SOUND_MUSIC)
		return;

	std::sprintf(filename, R"(cd\track%2.2d.ogg)", current_cd_track);
	ret = playSoundThreaded(filename, SOUND_MUSIC, cd_play_loop_flag);
	if (ret == SOUND_MUSIC)
		return;

	std::sprintf(filename, R"(cd\track%2.2d.wav)", current_cd_track);
	playSoundThreaded(filename, SOUND_MUSIC, cd_play_loop_flag, MIX_BGM_CHANNEL);
}

int ONScripter::playWave(const std::shared_ptr<Wrapped_Mix_Chunk> &chunk, int format, bool loop_flag, int channel) {
	// Both cache hits and decoded chunks publish here. Keep the old chunk
	// alive until playback is paused, and serialize publication with stops.
	Lock lock(&playSoundThreadedLock);
	Mix_Pause(channel);
	wave_sample[channel] = chunk;

	if (!chunk)
		return -1;

	if (channel < ONS_MIX_CHANNELS)
		setVolume(channel, channelvolumes[channel], volume_on_flag);
	else if (channel == MIX_BGM_CHANNEL)
		setVolume(MIX_BGM_CHANNEL, music_volume, volume_on_flag);
	else
		setVolume(channel, se_volume, volume_on_flag);

	if (lipsChannels[channel].has())
		loadLips(channel);

	if (!(format & SOUND_PRELOAD)) {
		if (lipsChannels[channel].has()) {
			LipsAnimationAction *lipsAction = LipsAnimationAction::create();
			lipsAction->channel             = channel;
			{
				Lock lock(&ons.registeredCRActions);
				registeredCRActions.emplace_back(lipsAction);
			}
			Mix_PlayChannel(channel, wave_sample[channel]->chunk, loop_flag ? -1 : 0);
			lipsChannels[channel].get().lipsData.speechStart = SDL_GetTicks();
		} else {
			Mix_PlayChannel(channel, wave_sample[channel]->chunk, loop_flag ? -1 : 0);
		}
	}

	return 0;
}

int ONScripter::playSequencedMusic(bool loop_flag) {
	Mix_SetMusicCMD(seqmusic_cmd);

	char seqmusic_filename[256];
	std::sprintf(seqmusic_filename, "%s%s", script_h.save_path, TMP_MUSIC_FILE);
	{
		Lock lock(&music_file_name); //general sound i/o lock
		seqmusic_info = Mix_LoadMUS(seqmusic_filename);
	}
	if (seqmusic_info == nullptr) {
		std::snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
		              "error in sequenced music file %s", seqmusic_filename);
		errorAndCont(script_h.errbuf, Mix_GetError());
		return -1;
	}

	int seqmusic_looping = loop_flag ? -1 : 0;

#ifdef LINUX
	signal(SIGCHLD, seqmusicCallback);
	if (seqmusic_cmd)
		seqmusic_looping = 0;
#endif
	setMusicVolume(music_volume, volume_on_flag);
	Mix_PlayMusic(seqmusic_info, seqmusic_looping);
	current_cd_track = -2;

	return 0;
}

int ONScripter::playingMusic() {
	return (audio_open_flag && (Mix_GetMusicHookData() || Mix_Playing(MIX_BGM_CHANNEL) == 1 || Mix_PlayingMusic() == 1)) ? 1 : 0;
}

int ONScripter::setCurMusicVolume(int volume) {
	if (!audio_open_flag)
		return 0;

	Lock lock(&playSoundThreadedLock);
	if (bgmdownmode_flag && music_info && wave_sample[0] && Mix_Playing(0))
		volume /= 2;
	if (Mix_Playing(MIX_BGM_CHANNEL) == 1) // wave/ogg (unstreamed)
		setVolume(MIX_BGM_CHANNEL, volume, volume_on_flag);
	else if (Mix_PlayingMusic() == 1) // mp3,ogg,midi,wave
		//FIXME: Can anybody tell me why this does not update music_volume?
		setMusicVolume(volume, volume_on_flag);

	return 0;
}

int ONScripter::setVolumeMute(bool do_mute) {
	if (!audio_open_flag)
		return 0;

	Lock lock(&playSoundThreadedLock);
	std::string title = wm_title_string;
	if (do_mute)
		title.insert(0, "[Sound: Off] ");
	window.setTitle(title.c_str());

	int music_vol = music_volume;
	if (bgmdownmode_flag && music_info && wave_sample[0] && Mix_Playing(0))
		music_vol /= 2;

	if (Mix_Playing(MIX_BGM_CHANNEL) == 1) // wave
		setVolume(MIX_BGM_CHANNEL, music_vol, !do_mute);
	else if (Mix_PlayingMusic() == 1) // mp3,ogg,midi
		setMusicVolume(music_vol, !do_mute);

	// Include the voice already playing on channel zero.
	for (uint32_t i = 0; i < ONS_MIX_CHANNELS; i++)
		setVolume(i, channelvolumes[i], !do_mute);

	setVolume(MIX_LOOPBGM_CHANNEL0, se_volume, !do_mute);
	setVolume(MIX_LOOPBGM_CHANNEL1, se_volume, !do_mute);
	setVolume(MIX_VIDEO_CHANNEL, video_volume, !do_mute);

	return 0;
}

void ONScripter::stopBGM(bool continue_flag) {
	// SDL mutexes are recursive: event dispatch may already hold this lock.
	// Take it before inspecting either the chunk or streamed-music pointers.
	Lock lock(&playSoundThreadedLock);
	if (wave_sample[MIX_BGM_CHANNEL]) {
		Mix_Pause(MIX_BGM_CHANNEL);
		wave_sample[MIX_BGM_CHANNEL] = nullptr;
	}

	if (music_info) {
		ext_music_play_once_flag = true;
		// Mix_HaltMusic in SDL2_mixer calls musicFinishCallback in the end, we want to avoid that
		// SDL12 backported that change in http://hg.libsdl.org/SDL_mixer/rev/a4e9c53d9c30
		// We use a prior rev atm but I add surrounding calls for both versions just in case.
		Mix_HookMusicFinished(nullptr);
		Mix_HaltMusic();
		Mix_HookMusicFinished(musicFinishCallback);
		Mix_FreeMusic(music_info);
		music_info = nullptr;
	}

	if (seqmusic_info) {
		ext_music_play_once_flag = true;
		// Mix_HaltMusic in SDL2_mixer calls musicFinishCallback in the end, we want to avoid that
		// SDL12 backported that change in http://hg.libsdl.org/SDL_mixer/rev/a4e9c53d9c30
		// We use a prior rev atm but I add surrounding calls for both versions just in case.
		Mix_HookMusicFinished(nullptr);
		Mix_HaltMusic();
		Mix_HookMusicFinished(musicFinishCallback);
		Mix_FreeMusic(seqmusic_info);
		seqmusic_info = nullptr;
	}

	if (!continue_flag) {
		script_h.setStr(&music_file_name, nullptr);
		music_play_loop_flag = false;
		if (initialised()) {
			if (music_buffer) {
				delete[] music_buffer;
				music_buffer = nullptr;
			}
		}

		script_h.setStr(&seqmusic_file_name, nullptr);
		seqmusic_play_loop_flag = false;

		current_cd_track = -1;
	}
}

void ONScripter::stopDWAVE(int channel) {
	if (!audio_open_flag)
		return;

	//avoid stopping dwave outside array
	if (channel < 0)
		channel = 0;
	else if (channel >= ONS_MIX_CHANNELS)
		channel = ONS_MIX_CHANNELS - 1;

	Lock lock(&playSoundThreadedLock);
	if (wave_sample[channel]) {
		Mix_Pause(channel);
		if (!channel_preloaded[channel]) {
			//don't free preloaded channels
			wave_sample[channel]       = nullptr;
			channel_preloaded[channel] = false;
		}
	}
	if ((channel == 0) && bgmdownmode_flag)
		setCurMusicVolume(music_volume);
}

void ONScripter::stopAllDWAVE() {
	if (!audio_open_flag)
		return;

	Lock lock(&playSoundThreadedLock);
	for (int ch = 0; ch < ONS_MIX_CHANNELS; ch++) {
		if (wave_sample[ch]) {
			Mix_Pause(ch);
			if (!channel_preloaded[ch]) {
				wave_sample[ch] = nullptr;
			}
		}
	}
	// just in case the bgm was turned down for the voice channel,
	// set the bgm volume back to normal
	if (bgmdownmode_flag)
		setCurMusicVolume(music_volume);
}

void ONScripter::playClickVoice() {
	if (clickstr_state == CLICK_NEWPAGE) {
		if (clickvoice_file_name[CLICKVOICE_NEWPAGE])
			playSoundThreaded(clickvoice_file_name[CLICKVOICE_NEWPAGE],
			                  SOUND_CHUNK, false, MIX_WAVE_CHANNEL);
	} else if (clickstr_state == CLICK_WAIT) {
		if (clickvoice_file_name[CLICKVOICE_NORMAL])
			playSoundThreaded(clickvoice_file_name[CLICKVOICE_NORMAL],
			                  SOUND_CHUNK, false, MIX_WAVE_CHANNEL);
	}
}

void ONScripter::startLvPlayback() {
	script_h.logState.currVoiceSet++;

	auto &data = script_h.logState.dialogueData[script_h.logState.currVoiceDialogueLabelIndex];

	if (data.voices.size() <= static_cast<size_t>(script_h.logState.currVoiceSet)) {
		stopLvPlayback();
		return;
	}

	auto &set   = data.voices[script_h.logState.currVoiceSet];
	auto vol    = data.volume * script_h.logState.currVoiceVolume / 100;
	int last_ch = -1;
	for (auto &voice : set) {
		stopDWAVE(voice.first);
		channel_preloaded[voice.first] = true;
		playSoundThreaded(voice.second.c_str(), SOUND_CHUNK | SOUND_PRELOAD, false, voice.first, false);
		setVolume(voice.first, vol, volume_on_flag);
		last_ch = voice.first;
	}

	auto action          = QueuedSoundAction::create();
	action->ch           = last_ch;
	action->func         = []() { ons.startLvPlayback(); };
	action->soundDelayMs = ignore_voicedelay ? 0 : voicedelay_time;

	{
		Lock lock(&ons.registeredCRActions);
		registeredCRActions.emplace_back(action);
	}

	// Do not hold this across the preload loop above: it waits for the
	// worker, which needs the same mutex to publish each decoded chunk.
	Lock lock(&playSoundThreadedLock);
	for (auto &voice : set) {
		if (wave_sample[voice.first] == nullptr)
			errorAndExit("Cannot play a not loaded channel");
		Mix_PlayChannel(voice.first, wave_sample[voice.first]->chunk, 0);
	}
}

void ONScripter::stopLvPlayback() {
	script_h.logState.currVoiceSet    = -1;
	script_h.logState.currVoiceVolume = 100;
	for (auto &act : getConstantRefreshActions()) {
		if (dynamic_cast<QueuedSoundAction *>(act.get())) {
			act->terminate();
		}
	}

	for (auto &set : script_h.logState.dialogueData[script_h.logState.currVoiceDialogueLabelIndex].voices) {
		for (auto &voice : set) {
			stopDWAVE(voice.first);
		}
	}
	script_h.logState.currVoiceDialogueLabelIndex = -1;
}

void setupWaveHeader(uint8_t *buffer, int channels, int bits,
                     unsigned long rate, size_t data_length,
                     size_t extra_bytes, uint8_t *extra_ptr) {
	WAVE_HEADER header;
	WAVE_DATA_HEADER data_header;
	std::memcpy(header.chunk_riff, "RIFF", 4);
	unsigned long riff_length = sizeof(WAVE_HEADER) + sizeof(WAVE_DATA_HEADER) +
	                            data_length + extra_bytes - 8;
	header.riff_length[0] = riff_length & 0xff;
	header.riff_length[1] = (riff_length >> 8) & 0xff;
	header.riff_length[2] = (riff_length >> 16) & 0xff;
	header.riff_length[3] = (riff_length >> 24) & 0xff;
	std::memcpy(header.fmt_id, "WAVEfmt ", 8);
	header.fmt_size[0] = 0x10 + extra_bytes;
	header.fmt_size[1] = header.fmt_size[2] = header.fmt_size[3] = 0;
	header.data_fmt[0]                                           = 1;
	header.data_fmt[1]                                           = 0; // PCM format
	header.channels[0]                                           = channels;
	header.channels[1]                                           = 0;
	header.frequency[0]                                          = rate & 0xff;
	header.frequency[1]                                          = (rate >> 8) & 0xff;
	header.frequency[2]                                          = (rate >> 16) & 0xff;
	header.frequency[3]                                          = (rate >> 24) & 0xff;

	int sample_byte_size       = channels * bits / 8;
	unsigned long byte_size    = sample_byte_size * rate;
	header.byte_size[0]        = byte_size & 0xff;
	header.byte_size[1]        = (byte_size >> 8) & 0xff;
	header.byte_size[2]        = (byte_size >> 16) & 0xff;
	header.byte_size[3]        = (byte_size >> 24) & 0xff;
	header.sample_byte_size[0] = sample_byte_size;
	header.sample_byte_size[1] = 0;
	header.sample_bit_size[0]  = bits;
	header.sample_bit_size[1]  = 0;

	std::memcpy(data_header.chunk_id, "data", 4);
	data_header.data_length[0] = static_cast<char>(data_length & 0xff);
	data_header.data_length[1] = static_cast<char>((data_length >> 8) & 0xff);
	data_header.data_length[2] = static_cast<char>((data_length >> 16) & 0xff);
	data_header.data_length[3] = static_cast<char>((data_length >> 24) & 0xff);

	std::memcpy(buffer, &header, sizeof(header));
	if (extra_bytes > 0) {
		if (extra_ptr != nullptr)
			std::memcpy(buffer + sizeof(header), extra_ptr, extra_bytes);
		else
			std::memset(buffer + sizeof(header), 0, extra_bytes);
	}
	std::memcpy(buffer + sizeof(header) + extra_bytes, &data_header, sizeof(data_header));
}

#ifdef VITA
// The next meplay loop, decoded ahead (see nativePrefetchAhead in
// Animation.cpp). dwaveload decodes a whole Ogg to the mixer format before
// the script continues: ~30 s 22 kHz mono loops become 5-13 MB of 48 kHz
// stereo PCM, which takes the Vita most of a second right before the
// transition that should start with the loop. Loops up to 6.5 MiB qualify.

// Decoded size of an Ogg Vorbis file in the mixer's format, from its headers,
// or 0 if it is not one we can tell.
static uint64_t nativeOggPcmBytes(const uint8_t *data, size_t length) {
	const uint8_t *id = nullptr;
	for (size_t i = 0; i + 16 <= length && i < 256 && !id; i++)
		if (data[i] == 1 && !std::memcmp(data + i + 1, "vorbis", 6))
			id = data + i;
	if (!id)
		return 0;
	const uint32_t rate = id[12] | (id[13] << 8) | (id[14] << 16) | (static_cast<uint32_t>(id[15]) << 24);
	// The last page's granule position is the stream's length in samples.
	int64_t samples = -1;
	for (size_t i = length >= 14 ? length - 14 : 0; i > 0 && samples < 0; i--) {
		if (std::memcmp(data + i, "OggS", 4) || data[i + 4] != 0)
			continue;
		uint64_t granule = 0;
		for (int b = 7; b >= 0; b--) granule = (granule << 8) | data[i + 6 + b];
		samples = static_cast<int64_t>(granule);
	}
	int freq{0}, channels{0};
	Uint16 format{0};
	if (!rate || samples <= 0 || !Mix_QuerySpec(&freq, &format, &channels))
		return 0;
	return static_cast<uint64_t>(samples) * static_cast<uint64_t>(freq) / rate * static_cast<uint64_t>(channels) *
	       (SDL_AUDIO_BITSIZE(format) / 8);
}

void ONScripter::nativeQueueLoopPrefetch(const std::string &file) {
	using State = NativePrefetch::State;
	if (!audio_open_flag)
		return;
	Lock cacheLock(&soundCache);
	auto &entry = nativeLoopPrefetch;
	if (!nativeLoopPrefetchSlot) {
		soundCache.makeLRU(entry.slot, 1);
		nativeLoopPrefetchSlot = true;
	}
	Lock lock(&nativeLoopPrefetch);
	if (entry.file == file && (entry.state == State::Queued || entry.state == State::Decoding ||
	                           (entry.state == State::Ready && soundCache.get(file))))
		return;
	if (entry.state == State::Decoding || soundCache.get(file))
		return;
	soundCache.clear(entry.slot); // A loop the script did not play after all.
	entry.file  = file;
	entry.state = State::Queued;
	// On the image cache thread with the picture prefetches: one prefetch at
	// a time, and the sound cache thread stays free for voice prefetches.
	async.cacheImage(entry.slot, file, false);
#if defined(ONS_VITA_FILE_LOGGING)
	sendToLog(LogLevel::Warn, "Timing %u: prefetch %s: queued\n", static_cast<unsigned>(SDL_GetTicks()), file.c_str());
#endif
}

// The loop's LoadImageCacheInstruction (nativeRunImagePrefetch), on the image
// cache thread. Decodes as playSound decodes a chunk, but with the file read
// in pieces that pause for script loads and with soft allocations.
bool ONScripter::nativeRunLoopPrefetch(const std::string &file) {
	using State = NativePrefetch::State;
	// Peak sound addition: one loop (umilse_012, 35 s, is 6.5 MiB decoded).
	constexpr uint64_t maxBytes = 6656 * 1024;
	auto &entry = nativeLoopPrefetch;
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
	while (nativePrefetchShouldYield() && !async.threadShutdownRequested)
		SDL_Delay(5);
	if (!nativeBeginPrefetch(entry, &nativeLoopPrefetch, file))
		return true;

#if defined(ONS_VITA_FILE_LOGGING)
	const auto decodeStart = SDL_GetTicks();
#endif
	const unsigned epoch = nativePrefetchEpoch;
	uint64_t pcmBytes{0};
	std::shared_ptr<Wrapped_Mix_Chunk> chunk;
	// From here on allocations fail (nullptr / std::bad_alloc) instead of
	// spilling into vitaGL's pools or reaching the fatal new-handler.
	VitaMemory::SoftAllocations soft;
	{
		Mix_Chunk *decoded{nullptr};
		try {
			size_t length{0};
			std::unique_ptr<FILE, int (*)(FILE *)> fp(nativePrefetchOpen(file, 2 * 1024 * 1024, length), std::fclose);
			std::unique_ptr<uint8_t[]> buffer;
			if (fp && nativePrefetchMemoryOk(length + 1024 * 1024, length)) {
				buffer.reset(new (std::nothrow) uint8_t[length]);
				if (buffer && !nativePrefetchRead(fp.get(), buffer.get(), length))
					buffer.reset();
			}
			fp.reset();
			if (buffer)
				pcmBytes = nativeOggPcmBytes(buffer.get(), length);
			// SDL_mixer holds up to about twice the PCM while it assembles it.
			if (pcmBytes && pcmBytes <= maxBytes &&
			    nativePrefetchMemoryOk(2 * pcmBytes + length + 1024 * 1024, static_cast<size_t>(pcmBytes))) {
				decoded = Mix_LoadWAV_RW(SDL_RWFromMem(buffer.get(), static_cast<int>(length)), 1);
				if (decoded)
					chunk = std::make_shared<Wrapped_Mix_Chunk>(decoded);
			}
		} catch (const std::bad_alloc &) {
			if (decoded && !chunk)
				Mix_FreeChunk(decoded); // Out of memory: skip, as if never queued.
		} catch (...) {
			Lock lock(&nativeLoopPrefetch);
			entry.state  = State::Empty;
			entry.thread = 0;
			entry.file.clear();
			throw;
		}
	}

	bool ready{false};
	{
		Lock cacheLock(&soundCache);
		// nativeReleasePrefetches may have run meanwhile: then drop it.
		if (chunk && epoch == nativePrefetchEpoch && !soundCache.get(file)) {
			try {
				soundCache.add(entry.slot, file, chunk);
				ready = true;
			} catch (const std::bad_alloc &) {
			}
		}
		Lock lock(&nativeLoopPrefetch);
		entry.state  = ready ? State::Ready : State::Empty;
		entry.thread = 0;
		if (!ready)
			entry.file.clear();
	}
#if defined(ONS_VITA_FILE_LOGGING)
	sendToLog(LogLevel::Warn, "Timing %u: prefetch %s: %s after %u ms (%u KiB decoded)\n",
	          static_cast<unsigned>(SDL_GetTicks()), file.c_str(), ready ? "decoded" : "dropped",
	          static_cast<unsigned>(SDL_GetTicks() - decodeStart), static_cast<unsigned>(pcmBytes / 1024));
#endif
	return true;
}

// playSoundThreaded, before its cache lookup: a queued prefetch of this file
// is dropped (this load decodes it now); one being decoded is awaited at this
// thread's priority, as finishing it takes less time than starting over.
void ONScripter::nativeAwaitPrefetchedLoop(const char *file, bool waitevent) {
	using State    = NativePrefetch::State;
	auto &entry    = nativeLoopPrefetch;
	const int self = sceKernelGetThreadId();
	bool boosted{false};
#if defined(ONS_VITA_FILE_LOGGING)
	const auto waitStart = SDL_GetTicks();
#endif
	while (true) {
		{
			Lock lock(&nativeLoopPrefetch);
			if (entry.file != file || entry.state == State::Empty || entry.state == State::Ready || entry.thread == self)
				break;
			if (entry.state == State::Queued) {
				entry.state = State::Empty;
				entry.file.clear();
#if defined(ONS_VITA_FILE_LOGGING)
				sendToLog(LogLevel::Warn, "Timing %u: prefetch %s: still queued, loading it now\n",
				          static_cast<unsigned>(SDL_GetTicks()), file);
#endif
				break;
			}
			if (!boosted) {
				nativePrefetchAwaited = true; // Its reads stop pausing for this load.
				sceKernelChangeThreadPriority(entry.thread, sceKernelGetThreadCurrentPriority());
				boosted = true;
				preventExit(true);
			}
		}
		if (waitevent) {
			event_mode = IDLE_EVENT_MODE;
			Lock lock(&playSoundThreadedLock);
			waitEvent(0);
		} else {
			SDL_Delay(1);
		}
	}
	if (boosted) {
		preventExit(false);
#if defined(ONS_VITA_FILE_LOGGING)
		sendToLog(LogLevel::Warn, "Timing %u: prefetch %s: waited %u ms for its decode\n",
		          static_cast<unsigned>(SDL_GetTicks()), file, static_cast<unsigned>(SDL_GetTicks() - waitStart));
#endif
	}
}

// trySoundCache found the file cached for a real channel, holding
// soundCache's lock. The loop is handed over once: the channel now holds the
// only reference, so the PCM goes when the loop stops, as without prefetching.
void ONScripter::nativeConsumePrefetchedLoop(const char *file) {
	using State = NativePrefetch::State;
	auto &entry = nativeLoopPrefetch;
	Lock lock(&nativeLoopPrefetch);
	// Decoding: cached already, the state not yet updated.
	if (entry.file != file || (entry.state != State::Ready && entry.state != State::Decoding))
		return;
	soundCache.clear(entry.slot);
	if (entry.state == State::Ready) {
		entry.state = State::Empty;
		entry.file.clear();
	}
#if defined(ONS_VITA_FILE_LOGGING)
	sendToLog(LogLevel::Warn, "Timing %u: prefetch %s: hit\n", static_cast<unsigned>(SDL_GetTicks()), file);
#endif
}
#endif
