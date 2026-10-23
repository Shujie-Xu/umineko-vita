/**
 *  Subtitle.hpp
 *  ONScripter-RU
 *
 *  Subtitle playback layer for any surfaces.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"
#include "Engine/Layers/Layer.hpp"
#include "Engine/Media/SubtitleDriver.hpp"
#include "Engine/Readers/Base.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_gpu.h>

#include <atomic>
#include <deque>
#include <memory>

class SubtitleLayer : public Layer {
	float ratio_x{0.75}, ratio_y{0.75};
	// ns per frame, current decoder position + decode_rate, current display position
	uint64_t decode_rate{0}, decoded_timestamp{0}, display_timestamp{0}; // All values are in ns
#ifdef VITA
	// display_timestamp for the decoding thread, which skips ahead to it
	// rather than render frames already due (see update()).
	std::atomic<uint64_t> shared_display_timestamp{0};
#endif
	bool startDecoding();
	void endDecoding();
	bool clockProceed();
	void renderImageSet(std::vector<SubtitleImage> &imgs);

	/* glUniformLocation arrays */
	int ntexturesHandle, dstDimsHandle, texHandle;
	int subDimsHandles[SubtitleDriver::NIMGS_MAX];
	int subCoordsHandles[SubtitleDriver::NIMGS_MAX];
	int subColorsHandles[SubtitleDriver::NIMGS_MAX];
#ifdef VITA
	// vitaGL resolves only whole parameters by name, never "name[i]", so the
	// handles above are all -1 there: each array is set at once instead.
	int subDimsArray{-1}, subCoordsArray{-1}, subColorsArray{-1};
#endif

	GPU_Image *subImages{nullptr};
#ifdef VITA
	// Stands in for current_frame as the source of the draw that composes a
	// frame into current_frame (see renderImageSet).
	GPU_Image *blitSource{nullptr};
	// The bitmap each subImages slot holds (SubtitleImage::hash), so that a
	// frame keeping it (every \fad step) does not upload it again.
	struct Slot {
		int w{-1}, h{-1};
		uint64_t hash{0};
	} subSlots[SubtitleDriver::NIMGS_MAX];
	// vitaGL frame in which a compose last sampled subImages (see renderImageSet).
	uint32_t subImagesFrame{0};
	bool subImagesSampled{false};
	// current_frame's storage size (render scale); the software fallback
	// blends to it (doDecoding).
	int framePhysicalW{0}, framePhysicalH{0};
	void replaceSubImages();
#endif
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	// Diagnostic builds: the main thread's cost of each subtitle change, with
	// the longest gap between updates over the next frames. The decoding
	// thread writes these to the log (logChanges), keeping the card writes
	// off the main thread.
	struct ChangeTiming {
		uint32_t atMs, uploadUs, composeUs, otherUs, gapUs;
		uint8_t images, uploaded, replaced, copied;
	};
	static constexpr unsigned ChangeRingSize{16};
	ChangeTiming changeRing[ChangeRingSize];
	std::atomic<unsigned> changeHead{0}, changeTail{0};
	ChangeTiming pendingChange{};
	unsigned pendingFrames{0};
	bool changePending{false};
	uint64_t lastUpdateCounter{0};
	uint32_t lastUploadUs{0}, lastComposeUs{0};
	uint8_t lastUploaded{0}, lastReplaced{0}, lastCopied{0};
	int mainThreadId{0};
	void noteChange(const ChangeTiming *change);
	void noteUpdateGap(uint64_t now);
	void logChanges();
#endif

	struct SubtitleFrame {
		uint64_t start_timestamp;
		std::vector<SubtitleImage> imgs;
		std::unique_ptr<uint8_t[]> sw_buffer; /* Used for fallback */
	};

	const size_t frameQueueMaxSize{10};
	std::deque<SubtitleFrame> frameQueue; //TODO: replace by a ring buffer
	SDL_mutex *frameQueueMutex{nullptr};

	SDL_semaphore *threadSemaphore{nullptr};
	std::atomic<bool> should_finish{false};
	bool playback{false};

	GPU_Image *current_frame{nullptr};
	int current_frame_format{0};
	uint64_t current_timestamp{0xFFFFFFFFFFFFFFFF};

public:
	SubtitleLayer(unsigned int w, unsigned int h, BaseReader **br, unsigned int scale_x = 100, unsigned int scale_y = 100);
	~SubtitleLayer() override;
	bool loadSubtitles(const char *filename, unsigned int rateMs);
	void stopPlayback();
	void setFont(unsigned int id);

	void doDecoding();
	bool update(bool /*old*/) override;
	void refresh(GPU_Target *target, GPU_Rect &clip, float x, float y, bool centre_coordinates, int /*rm*/, float /*scalex*/, float /*scaley*/) override;
	BlendModeId blendingMode(int rm) override {
		return blendingModeSupported(rm);
	}

	SubtitleDriver subtitleDriver; //-V730_NOINIT

	Clock mediaClock;
	uint64_t nanosPerFrame{0};
};
