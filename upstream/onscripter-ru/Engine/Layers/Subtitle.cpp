/**
 *  Subtitle.cpp
 *  ONScripter-RU
 *
 *  Subtitle playback layer for any surfaces.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Layers/Subtitle.hpp"
#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Async.hpp"

#ifdef VITA
#include <algorithm>
// vitaGL's count of presented frames (gxm.c). glTexSubImage2D copies a whole
// texture first when it was sampled within FRAME_PURGE_FREQ frames of it.
extern "C" uint32_t vgl_framecount;
static constexpr uint32_t VitaFramePurgeFreq{4};
#if defined(ONS_VITA_FILE_LOGGING)
#include "platform/vita/platform.hpp"
#include <psp2/kernel/threadmgr.h>
static uint32_t microsecondsSince(uint64_t start) {
	return static_cast<uint32_t>((SDL_GetPerformanceCounter() - start) * 1000000 / SDL_GetPerformanceFrequency());
}
#endif
#endif

/* Subtitle wrapper */

SubtitleLayer::SubtitleLayer(unsigned int w, unsigned int h, BaseReader **br, unsigned int scale_x, unsigned int scale_y) {
	if (scale_x <= 100)
		ratio_x = scale_x / 100.0f;
	if (scale_y <= 100)
		ratio_y = scale_y / 100.0f;

	width  = w * ratio_x;
	height = h * ratio_y;

#ifdef VITA
	// loadSubtitles makes current_frame when a script first needs it: a
	// full-size frame here held 8 MB of video memory from startup on.
#else
	current_frame = gpu.createImage(w, h, 4);
	GPU_GetTarget(current_frame);
	current_frame_format = (gpu.*gpu.current_renderer->getImageFormat)(current_frame);
#endif
	reader = br;

	/* Get shader uniform loc */
	gpu.setShaderProgram("renderSubtitles.frag");
	ntexturesHandle = gpu.getUniformLoc("ntextures");
	dstDimsHandle   = gpu.getUniformLoc("dstDims");
	texHandle       = gpu.getUniformLoc("subTex");

	for (size_t i = 0; i < SubtitleDriver::NIMGS_MAX; i++) {
		char nameBuf[32];

		std::snprintf(nameBuf, sizeof(nameBuf), "subDims[%u]", static_cast<unsigned>(i));
		subDimsHandles[i] = gpu.getUniformLoc(nameBuf);

		std::snprintf(nameBuf, sizeof(nameBuf), "subCoords[%u]", static_cast<unsigned>(i));
		subCoordsHandles[i] = gpu.getUniformLoc(nameBuf);

		std::snprintf(nameBuf, sizeof(nameBuf), "subColors[%u]", static_cast<unsigned>(i));
		subColorsHandles[i] = gpu.getUniformLoc(nameBuf);
	}
#ifdef VITA
	subDimsArray   = gpu.getUniformLoc("subDims");
	subCoordsArray = gpu.getUniformLoc("subCoords");
	subColorsArray = gpu.getUniformLoc("subColors");
#if defined(ONS_VITA_FILE_LOGGING)
	sendToLog(LogLevel::Warn, "Subtitles: uniform arrays subDims %d subCoords %d subColors %d, element subDims[1] %d\n",
	          subDimsArray, subCoordsArray, subColorsArray, subDimsHandles[1]);
#endif
#endif
	gpu.unsetShaderProgram();

	/* Prepare images */
	subImages = gpu.createImage(SubtitleDriver::IMG_W, SubtitleDriver::IMG_H * SubtitleDriver::NIMGS_MAX, 1);
	GPU_SetBlending(subImages, false);
#ifdef VITA
	blitSource = gpu.createImage(16, 16, 4);
#endif
}

SubtitleLayer::~SubtitleLayer() {
	// Required for e.g. definereset.
	stopPlayback();

	gpu.freeImage(subImages);
#ifdef VITA
	gpu.freeImage(blitSource);
#endif
}

bool SubtitleLayer::loadSubtitles(const char *filename, unsigned int rateMs) {
	stopPlayback();

	if (!subtitleDriver.init(width, height, filename, *reader, nullptr)) {
		sendToLog(LogLevel::Error, "ass.dll: Failed to load %s\n", filename);
		return false;
	}

	if (!current_frame) {
#ifdef VITA
		// At render scale, like the game's other targets. At 1920x1080 the
		// compose and the per-frame draw of this frame did three times the GPU
		// work for the pixels that reach the screen. The logical size, and so
		// every placement, is unchanged.
		current_frame        = gpu.createTargetImage(width, height, 4);
		current_frame_format = (gpu.*gpu.current_renderer->getImageFormat)(current_frame);
		framePhysicalW       = current_frame->base_w;
		framePhysicalH       = current_frame->base_h;
#else
		current_frame = gpu.createImage(width, height, 4);
		GPU_GetTarget(current_frame);
#endif
	}

	decode_rate = static_cast<uint64_t>(rateMs) * 1000000;

	return startDecoding();
}

void SubtitleLayer::stopPlayback() {
	endDecoding();
	frameQueue.clear();
	subtitleDriver.deinit();
	if (current_frame)
		gpu.freeImage(current_frame);
	current_frame = nullptr;
}

void SubtitleLayer::setFont(unsigned int id) {
	subtitleDriver.setFont(id);
}

void SubtitleLayer::refresh(GPU_Target *target, GPU_Rect &clip, float x, float y, bool /*centre_coordinates*/, int /*rm*/, float /*scalex*/, float /*scaley*/) {
	if (current_frame)
		gpu.copyGPUImage(current_frame, nullptr, &clip, target,
		                 width / ratio_x / 2.0 + x, height / ratio_y / 2.0 + y,
		                 1 / ratio_x, 1 / ratio_y, 0, true);
}

bool SubtitleLayer::update(bool /*old*/) {
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	const uint64_t updateStart = SDL_GetPerformanceCounter();
	noteUpdateGap(updateStart);
#endif
	if (!clockProceed())
		return true;

#ifdef VITA
	// Never hold the frame for the decoding thread. libass can take longer
	// than a frame to render a changing lyric line on the Vita, and waiting
	// for it stalled the whole game (about 100 ms every couple of seconds
	// while the ending credits scroll). Show the newest frame that is ready;
	// the decoder skips ahead to this timestamp (doDecoding).
	shared_display_timestamp.store(display_timestamp, std::memory_order_relaxed);
	SDL_LockMutex(frameQueueMutex);
#else
	SDL_LockMutex(frameQueueMutex);

	//CHECKME: may it deadlock here on quit?
	while (decoded_timestamp < decode_rate || decoded_timestamp - decode_rate < display_timestamp) {
		if (frameQueue.size() == frameQueueMaxSize)
			break;
		SDL_UnlockMutex(frameQueueMutex);
		SDL_Delay(1); //TODO: replace by semaphores
		SDL_LockMutex(frameQueueMutex);
	}
#endif

	// This is possible, when there are no frames in the beginning
	if (frameQueue.empty()) {
		SDL_UnlockMutex(frameQueueMutex);
		return true;
	}

	auto it = frameQueue.begin();

	while (it != frameQueue.end()) {
		if (it->start_timestamp <= display_timestamp) {
			if (it + 1 != frameQueue.end()) {
				++it;
				continue;
			}
		} else {
			--it;
		}
		break;
	}

	assert(it != frameQueue.end());

	std::unique_ptr<uint8_t[]> needed_buffer;
	std::vector<SubtitleImage> imgs;
	bool gotFrame{false};
	if (it->start_timestamp != current_timestamp) {
		if (!it->imgs.empty()) {
			imgs = std::move(it->imgs);
		} else {
			needed_buffer = std::move(it->sw_buffer);
		}
		gotFrame          = true;
		current_timestamp = it->start_timestamp;
	}

	if (it - frameQueue.begin() > 1)
		frameQueue.erase(frameQueue.begin(), it - 1);

	SDL_UnlockMutex(frameQueueMutex);

#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	lastUploadUs = lastComposeUs = 0;
	lastUploaded = lastReplaced = lastCopied = 0;
	const auto images = static_cast<uint8_t>(imgs.size());
#endif
	if (current_frame) {
		if (!imgs.empty()) {
			renderImageSet(imgs);
		} else if (needed_buffer) {
#ifdef VITA
#if defined(ONS_VITA_FILE_LOGGING)
			const uint64_t uploadStart = SDL_GetPerformanceCounter();
#endif
			// Blended at current_frame's storage size (doDecoding).
			GPU_UpdateImageBytes(current_frame, nullptr, needed_buffer.get(), framePhysicalW * 4);
#if defined(ONS_VITA_FILE_LOGGING)
			lastUploadUs = microsecondsSince(uploadStart);
#endif
#else
			GPU_UpdateImageBytes(current_frame, nullptr, needed_buffer.get(), width * 4);
#endif
		} else if (gotFrame) {
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
			const uint64_t clearStart = SDL_GetPerformanceCounter();
			gpu.clearWholeTarget(current_frame->target);
			lastComposeUs = microsecondsSince(clearStart);
#else
			gpu.clearWholeTarget(current_frame->target);
#endif
		}
	}
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	if (gotFrame && current_frame) {
		ChangeTiming change{};
		change.atMs      = static_cast<uint32_t>(current_timestamp / 1000000);
		change.uploadUs  = lastUploadUs;
		change.composeUs = lastComposeUs;
		const uint32_t totalUs{microsecondsSince(updateStart)};
		change.otherUs  = totalUs > lastUploadUs + lastComposeUs ? totalUs - lastUploadUs - lastComposeUs : 0;
		change.images   = images;
		change.uploaded = lastUploaded;
		change.replaced = lastReplaced;
		change.copied   = lastCopied;
		noteChange(&change);
	}
#endif
	return true;
}

/* Private Subtitle wrapper */

void SubtitleLayer::renderImageSet(std::vector<SubtitleImage> &imgs) {
#ifdef VITA
#if defined(ONS_VITA_FILE_LOGGING)
	const uint64_t uploadStart = SDL_GetPerformanceCounter();
#endif
	// Upload only slots whose bitmap changed. A \fad step keeps every bitmap
	// and changes only the colours, which are uniforms (143 of the 201 changes
	// in the Episode 8 credits).
	auto holds = [this](size_t i, const SubtitleImage &img) {
		return subSlots[i].w == img.w && subSlots[i].h == img.h && subSlots[i].hash == img.hash;
	};
	bool uploads{false};
	for (size_t i = 0; i < imgs.size(); i++)
		uploads |= !holds(i, imgs[i]);
	// An upload to a texture sampled within the last few frames makes vitaGL
	// copy all of it first (4 MiB, read back from video memory) in case the GPU
	// still reads it. A change straight after another (a line after a fade)
	// takes a new zeroed texture instead and uploads every slot.
	if (uploads && subImagesSampled && vgl_framecount - subImagesFrame <= VitaFramePurgeFreq)
		replaceSubImages();
#if defined(ONS_VITA_FILE_LOGGING)
	void *storage = GPUController::textureStorage(subImages);
#endif
	for (size_t i = 0; i < imgs.size(); i++) {
		if (holds(i, imgs[i]))
			continue;
		// With the transparent column and row extractFrame adds, inside the slot.
		GPU_Rect rect;
		rect.x = 0;
		rect.y = i * SubtitleDriver::IMG_H;
		rect.w = std::min(imgs[i].w + 1, SubtitleDriver::IMG_W);
		rect.h = std::min(imgs[i].h + 1, SubtitleDriver::IMG_H);
		GPU_UpdateImageBytes(subImages, &rect, imgs[i].buffer.get(), imgs[i].linesize);
		subSlots[i] = {imgs[i].w, imgs[i].h, imgs[i].hash};
#if defined(ONS_VITA_FILE_LOGGING)
		lastUploaded++;
#endif
	}
#if defined(ONS_VITA_FILE_LOGGING)
	lastCopied   = lastUploaded && GPUController::textureStorage(subImages) != storage;
	lastUploadUs = microsecondsSince(uploadStart);
	const uint64_t composeStart = SDL_GetPerformanceCounter();
#endif
	// Only the lines need composing. They sit in opposite corners, so their
	// union is nearly the whole frame, but they cover a few percent of it.
	// Group each line's images (fill, border, shadow) into one area; the rest
	// of the frame is cleared. Every area still evaluates all the images, so
	// pixels where areas overlap get the same value twice.
	struct Area {
		float x0, y0, x1, y1;
	} areas[SubtitleDriver::NIMGS_MAX];
	const float frameW = current_frame->w, frameH = current_frame->h;
	size_t nareas{0};
	for (auto &img : imgs) {
		// A pixel of margin keeps the edges filtered at render scale.
		Area area{std::max(0.0f, img.x - 1.0f), std::max(0.0f, img.y - 1.0f),
		          std::min(frameW, img.x + img.w + 1.0f), std::min(frameH, img.y + img.h + 1.0f)};
		if (area.x0 < area.x1 && area.y0 < area.y1 && nareas < SubtitleDriver::NIMGS_MAX)
			areas[nareas++] = area;
	}
	for (bool merged{true}; merged;) {
		merged = false;
		for (size_t i = 0; i < nareas && !merged; i++) {
			for (size_t j = i + 1; j < nareas && !merged; j++) {
				Area &a = areas[i], &b = areas[j];
				if (a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1) {
					a      = {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
					b      = areas[--nareas];
					merged = true;
				}
			}
		}
	}
	// Before the program is set: a clear may be drawn as a shape.
	gpu.clearWholeTarget(current_frame->target);
#else
	for (size_t i = 0; i < imgs.size(); i++) {
		GPU_Rect rect;
		rect.x = 0;
		rect.y = i * SubtitleDriver::IMG_H;
		rect.h = imgs[i].h;
		rect.w = imgs[i].w;

		GPU_UpdateImageBytes(subImages, &rect, imgs[i].buffer.get(), imgs[i].linesize);
	}
#endif

	gpu.setShaderProgram("renderSubtitles.frag");

	GPU_SetUniformi(ntexturesHandle, static_cast<int>(imgs.size()));
	float dstDims[]{static_cast<float>(current_frame->w), static_cast<float>(current_frame->h)};
	GPU_SetUniformfv(dstDimsHandle, 2, 1, dstDims);

	GPU_SetShaderImage(subImages, texHandle, 1);

#ifdef VITA
	// One call per array (see subDimsArray): with per-element handles of -1
	// every lyric line had zero size and colour, so none ever showed.
	float dims[2 * SubtitleDriver::NIMGS_MAX]{}, coords[2 * SubtitleDriver::NIMGS_MAX]{},
	    colors[4 * SubtitleDriver::NIMGS_MAX]{};
	for (size_t i = 0; i < imgs.size(); i++) {
		dims[2 * i]       = static_cast<float>(imgs[i].w);
		dims[2 * i + 1]   = static_cast<float>(imgs[i].h);
		coords[2 * i]     = static_cast<float>(imgs[i].x);
		coords[2 * i + 1] = static_cast<float>(imgs[i].y);
		colors[4 * i]     = (imgs[i].color >> 24) / 255.0f;
		colors[4 * i + 1] = ((imgs[i].color >> 16) & 0xff) / 255.0f;
		colors[4 * i + 2] = ((imgs[i].color >> 8) & 0xff) / 255.0f;
		colors[4 * i + 3] = (255 - (imgs[i].color & 0xFF)) / 255.0f;
	}
	const int count = static_cast<int>(imgs.size());
	GPU_SetUniformfv(subDimsArray, 2, count, dims);
	GPU_SetUniformfv(subCoordsArray, 2, count, coords);
	GPU_SetUniformfv(subColorsArray, 4, count, colors);
#else
	for (size_t i = 0; i < imgs.size(); i++) {
		float subDims[]{static_cast<float>(imgs[i].w), static_cast<float>(imgs[i].h)};
		GPU_SetUniformfv(subDimsHandles[i], 2, 1, subDims);

		float subCoords[]{static_cast<float>(imgs[i].x), static_cast<float>(imgs[i].y)};
		GPU_SetUniformfv(subCoordsHandles[i], 2, 1, subCoords);

		float subColors[]{(imgs[i].color >> 24) / 255.0f,
		                  ((imgs[i].color >> 16) & 0xff) / 255.0f,
		                  ((imgs[i].color >> 8) & 0xff) / 255.0f,
		                  (255 - (imgs[i].color & 0xFF)) / 255.0f};
		GPU_SetUniformfv(subColorsHandles[i], 4, 1, subColors);
	}
#endif

#ifdef VITA
	// The shader reads only subTex; the drawn image just spans the frame. Use
	// a stand-in stretched over it: drawing current_frame into its own target
	// binds a texture as both source and render attachment, which GXM does
	// not support (graphics.md, render-to-self). A new lyric line stalled the
	// game for about 100 ms and the lyrics never appeared.
	// Centre coordinates: for a corner, copyGPUImage adds half the unscaled
	// size, which would leave the stretched quad centred near the origin.
	// Each area draws the matching part of the stretched stand-in, so the
	// shader's texture coordinates still map onto the whole frame.
	GPU_SetBlending(blitSource, false);
	for (size_t i = 0; i < nareas; i++) {
		const Area &area = areas[i];
		GPU_Rect source{area.x0 / frameW * blitSource->w, area.y0 / frameH * blitSource->h,
		                (area.x1 - area.x0) / frameW * blitSource->w, (area.y1 - area.y0) / frameH * blitSource->h};
		gpu.copyGPUImage(blitSource, &source, nullptr, current_frame->target, (area.x0 + area.x1) / 2.0f,
		                 (area.y0 + area.y1) / 2.0f, frameW / blitSource->w, frameH / blitSource->h, 0, true);
	}
	GPU_SetBlending(blitSource, true);
	subImagesFrame   = vgl_framecount;
	subImagesSampled = true;
#else
	// This is fine and does not need gpu.render_to_self guard because we do not read/write
	// current_frame pixels simultaneously.
	GPU_SetBlending(current_frame, false);
	gpu.copyGPUImage(current_frame, nullptr, nullptr, current_frame->target);
	GPU_SetBlending(current_frame, true);
#endif

	gpu.unsetShaderProgram();
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	// Switching programs submitted the batched quads (CPU side only).
	lastComposeUs = microsecondsSince(composeStart);
#endif
}

#ifdef VITA
void SubtitleLayer::replaceSubImages() {
	GPU_Image *fresh = GPU_CreateImage(SubtitleDriver::IMG_W, SubtitleDriver::IMG_H * SubtitleDriver::NIMGS_MAX,
	                                   GPU_FORMAT_LUMINANCE);
	if (!fresh || !GPUController::hasTextureStorage(fresh)) {
		// Low on memory: let vitaGL copy the texture as before.
		if (fresh)
			GPU_FreeImage(fresh);
		return;
	}
	GPU_SetSnapMode(fresh, GPU_SNAP_NONE);
	GPU_SetBlending(fresh, false);
	// vitaGL frees the old storage once the GPU has finished with it.
	gpu.freeImage(subImages);
	subImages = fresh;
	for (auto &slot : subSlots)
		slot = Slot{};
	subImagesSampled = false;
#if defined(ONS_VITA_FILE_LOGGING)
	lastReplaced = 1;
#endif
}

// Box filter from the logical frame to current_frame's storage size. The
// pixels are premultiplied, so averaging them is exact.
static void reduceToStorage(const uint8_t *src, int srcW, int srcH, uint8_t *dst, int dstW, int dstH) {
	for (int y = 0; y < dstH; y++) {
		const int y0 = y * srcH / dstH, y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
		for (int x = 0; x < dstW; x++) {
			const int x0 = x * srcW / dstW, x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
			unsigned sum[4]{};
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++)
					for (int c = 0; c < 4; c++)
						sum[c] += src[(static_cast<size_t>(sy) * srcW + sx) * 4 + c];
			const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
			for (int c = 0; c < 4; c++)
				dst[(static_cast<size_t>(y) * dstW + x) * 4 + c] = static_cast<uint8_t>(sum[c] / count);
		}
	}
}
#endif

#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
// Main thread: hands the pending change, if any, to the decoding thread and
// starts collecting update gaps for the new one (nullptr: none).
void SubtitleLayer::noteChange(const ChangeTiming *change) {
	if (changePending) {
		const unsigned head = changeHead.load(std::memory_order_relaxed);
		if (head - changeTail.load(std::memory_order_acquire) < ChangeRingSize) {
			changeRing[head % ChangeRingSize] = pendingChange;
			changeHead.store(head + 1, std::memory_order_release);
		}
	}
	changePending = change != nullptr;
	if (change) {
		pendingChange = *change;
		pendingFrames = 0;
	}
}

void SubtitleLayer::noteUpdateGap(uint64_t now) {
	const uint64_t previous = lastUpdateCounter;
	lastUpdateCounter       = now;
	if (!changePending || !previous)
		return;
	const auto gapUs    = static_cast<uint32_t>((now - previous) * 1000000 / SDL_GetPerformanceFrequency());
	pendingChange.gapUs = std::max(pendingChange.gapUs, gapUs);
	// The change's own frame, then three more (a GPU stall shows a frame late).
	if (++pendingFrames == 4)
		noteChange(nullptr);
}

// Decoding thread: writes what the main thread recorded (see update()).
void SubtitleLayer::logChanges() {
	static unsigned logged{0};
	const unsigned head = changeHead.load(std::memory_order_acquire);
	unsigned tail       = changeTail.load(std::memory_order_relaxed);
	for (; tail != head; tail++) {
		const ChangeTiming change = changeRing[tail % ChangeRingSize];
		if (logged++ >= 400)
			continue;
		if (!change.images && change.uploadUs)
			sendToLog(LogLevel::Warn,
			          "Subtitles: change at %u ms on the main thread: software frame upload %u us, other %u us; "
			          "longest update gap over the next 4 frames %u us\n",
			          change.atMs, change.uploadUs, change.otherUs, change.gapUs);
		else
			sendToLog(LogLevel::Warn,
			          "Subtitles: change at %u ms on the main thread: upload %u us (%u of %u images%s%s), "
			          "compose %u us, other %u us; longest update gap over the next 4 frames %u us\n",
			          change.atMs, change.uploadUs, static_cast<unsigned>(change.uploaded),
			          static_cast<unsigned>(change.images), change.replaced ? ", new texture" : "",
			          change.copied ? ", vitaGL copied the texture" : "", change.composeUs, change.otherUs,
			          change.gapUs);
	}
	changeTail.store(tail, std::memory_order_release);
}
#endif

void SubtitleLayer::doDecoding() {
#ifdef VITA
	// libass needs 50-125 ms for a changing lyric line here. At the creating
	// thread's priority, on a shared core, that held the game's frames back;
	// the decoder runs seconds ahead, so it can wait for idle time.
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
#if defined(ONS_VITA_FILE_LOGGING)
	VitaPlatform::logThreadPlacement("main (subtitle start)", mainThreadId);
	VitaPlatform::logThreadPlacement("subtitle decoder", sceKernelGetThreadId());
#endif
#endif
	while (true) {
		while (!should_finish.load(std::memory_order_acquire)) {
			if (async.threadShutdownRequested) {
				should_finish.store(true, std::memory_order_release);
				break;
			}

#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
			logChanges();
#endif
			SDL_mutexP(frameQueueMutex);
			if (frameQueue.size() >= frameQueueMaxSize) {
				SDL_mutexV(frameQueueMutex);
				SDL_Delay(1); //TODO: replace by semaphores
			} else {
				SDL_mutexV(frameQueueMutex);
				break;
			}
		}

		// Finish if requested
		if (should_finish.load(std::memory_order_acquire))
			break;

#ifdef VITA
		// Behind the display (see update()): skip the frames already due.
		const uint64_t shown = shared_display_timestamp.load(std::memory_order_relaxed);
		if (decode_rate && decoded_timestamp + decode_rate <= shown)
			decoded_timestamp = shown - shown % decode_rate;
#endif
		bool frameReady{false};
		SubtitleFrame frame;
		frame.start_timestamp = decoded_timestamp;

#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
		const auto renderStart = SDL_GetTicks();
#endif
		ASS_Image *img;
		int changed = subtitleDriver.extractFrame(frame.imgs, decoded_timestamp / 1000000, &img);
#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
		// Diagnostic builds: libass frames slower than the 30 ms frame rate.
		static unsigned slowFrames{0};
		const auto renderMs = SDL_GetTicks() - renderStart;
		if (renderMs >= 30 && slowFrames++ < 60)
			sendToLog(LogLevel::Warn, "Subtitles: the frame at %u ms took %u ms to render (display at %u ms)\n",
			          static_cast<unsigned>(decoded_timestamp / 1000000), static_cast<unsigned>(renderMs),
			          static_cast<unsigned>(shown / 1000000));
#endif
		if (changed >= 0) {
			frameReady = changed > 0;
		} else {
			const size_t frame_size  = width * height * 4;
			frame.sw_buffer = std::make_unique<uint8_t[]>(frame_size);
			subtitleDriver.blendBufInNeed(frame.sw_buffer.get(), width, current_frame_format, img);
#ifdef VITA
			// current_frame is stored at render scale (loadSubtitles).
			if (framePhysicalW != static_cast<int>(width) || framePhysicalH != static_cast<int>(height)) {
				auto reduced = std::make_unique<uint8_t[]>(static_cast<size_t>(framePhysicalW) * framePhysicalH * 4);
				reduceToStorage(frame.sw_buffer.get(), static_cast<int>(width), static_cast<int>(height), reduced.get(),
				                framePhysicalW, framePhysicalH);
				frame.sw_buffer = std::move(reduced);
			}
#endif
			frameReady = true;
		}

		if (frameReady) {
			SDL_mutexP(frameQueueMutex);
			frameQueue.emplace_back(std::move(frame));
			SDL_mutexV(frameQueueMutex);
		}

		decoded_timestamp += decode_rate;
	}

#if defined(VITA) && defined(ONS_VITA_FILE_LOGGING)
	logChanges();
	VitaPlatform::logThreadPlacement("main (subtitle end)", mainThreadId);
	VitaPlatform::logThreadPlacement("subtitle decoder", sceKernelGetThreadId());
#endif
	SDL_SemPost(threadSemaphore);
}

bool SubtitleLayer::startDecoding() {
	if (!frameQueueMutex)
		frameQueueMutex = SDL_CreateMutex();
	if (!threadSemaphore)
		threadSemaphore = SDL_CreateSemaphore(0);

	decoded_timestamp = 0;
	display_timestamp = 0;
#ifdef VITA
	shared_display_timestamp.store(0, std::memory_order_relaxed);
#if defined(ONS_VITA_FILE_LOGGING)
	mainThreadId = sceKernelGetThreadId();
#endif
#endif
	mediaClock.reset();
	nanosPerFrame = 1000000000 / (ons.game_fps ? ons.game_fps : DEFAULT_FPS);
	frameQueue.emplace_back(); // zero frame

	async.loadSubtitleFrames(this);

	playback = true;

	return false;
}

void SubtitleLayer::endDecoding() {
	if (threadSemaphore) {
		should_finish.store(true, std::memory_order_release);
		SDL_SemWait(threadSemaphore);
		SDL_DestroySemaphore(threadSemaphore);
		threadSemaphore = nullptr;
		should_finish.store(false, std::memory_order_seq_cst);
	}

	playback = false;

	if (frameQueueMutex)
		SDL_DestroyMutex(frameQueueMutex);
	frameQueueMutex = nullptr;
}

bool SubtitleLayer::clockProceed() {
	if (!playback || !sprite)
		return false;

	int framesToAdvance{0};

	mediaClock.tickNanos(sprite->clock.lapNanos());

	if (!mediaClock.hasCountdown()) {
		mediaClock.addCountdownNanos(nanosPerFrame);
		mediaClock.tickNanos(nanosPerFrame); // make the first frame immediately ready (off-by-1 fix)
	}
	while (mediaClock.expired()) {
		mediaClock.addCountdownNanos(nanosPerFrame);
		framesToAdvance++;
	}

	if (framesToAdvance == 0)
		return false;

	display_timestamp += nanosPerFrame * framesToAdvance;

	return true;
}
