//
// FPlayAndroid is distributed under the FreeBSD License
//
// Copyright (c) 2013-2014, Carlos Rafael Gimenes das Neves
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The views and conclusions contained in the software and documentation are those
// of the authors and should not be interpreted as representing official policies,
// either expressed or implied, of the FreeBSD Project.
//
// https://github.com/carlosrafaelgn/FPlayAndroid
//

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>

//http://developer.android.com/intl/pt-br/ndk/guides/audio/opensl-for-android.html#da
//... deprecated Android-specific extension to OpenSL ES 1.0.1 for decoding an encoded stream to PCM without immediate playback
//DECODING AUDIO WITH OPENSL ES IS DEPRECATED IN ANDROID!!!
//To decode an encoded stream to PCM but not play back immediately, for apps running on Android 4.x (API levels 16–20), we
//recommend using the MediaCodec class. For new applications running on Android 5.0 (API level 21) or higher, we recommend
//using the NDK equivalent, <NdkMedia*.h>. These header files reside in the media/ directory under your installation root.

#define INPUT_BUFFER_TIMEOUT_IN_US 2500
#define OUTPUT_BUFFER_TIMEOUT_IN_US 35000

class MediaCodec {
public:
	unsigned char* buffer;

private:
	int inputOver;
	ssize_t bufferIndex;
	AMediaExtractor* mediaExtractor;
	AMediaCodec* mediaCodec;

	static bool isAudio(const char* mime) {
		return (mime &&
			(mime[0] == 'a') &&
			(mime[1] == 'u') &&
			(mime[2] == 'd') &&
			(mime[3] == 'i') &&
			(mime[4] == 'o') &&
			(mime[5] == '/'));
	}

	int fillInputBuffers() {
		if (inputOver)
			return 0;
		for (int i = 0; i < 16 && !inputOver; i++) {
			const ssize_t index = AMediaCodec_dequeueInputBuffer(mediaCodec, INPUT_BUFFER_TIMEOUT_IN_US);
			if (index < 0)
				break;
			size_t inputBufferCapacity;
			unsigned char* inputBuffer = AMediaCodec_getInputBuffer(mediaCodec, index, &inputBufferCapacity);
			if (!inputBuffer)
				break;
			ssize_t size = AMediaExtractor_readSampleData(mediaExtractor, inputBuffer, inputBufferCapacity);
			if (size < 0) {
				inputOver = true;
				int ret;
				if ((ret = AMediaCodec_queueInputBuffer(mediaCodec, index, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)))
					return ret;
				break;
			} else {
				int ret;
				if ((ret = AMediaCodec_queueInputBuffer(mediaCodec, index, 0, size, 0, 0)))
					return ret;
				//although the doc says "Returns false if no more sample data is available
				//(end of stream)", sometimes, AMediaExtractor_advance() returns false in other cases....
				AMediaExtractor_advance(mediaExtractor);
			}
		}
		return 0;
	}

public:
	MediaCodec() {
		inputOver = false;
		bufferIndex = -1;
		buffer = 0;
		mediaExtractor = 0;
		mediaCodec = 0;
	}

	~MediaCodec() {
		inputOver = false;
		bufferIndex = -1;
		buffer = 0;
		if (mediaExtractor) {
			AMediaExtractor_delete(mediaExtractor);
			mediaExtractor = 0;
		}
		if (mediaCodec) {
			AMediaCodec_stop(mediaCodec);
			AMediaCodec_delete(mediaCodec);
			mediaCodec = 0;
		}
	}

	int prepare(int fd, uint64_t length, uint64_t* outParams) {
		int ret;

		mediaExtractor = AMediaExtractor_new();
		if (!mediaExtractor)
			return -1;

		if ((ret = AMediaExtractor_setDataSourceFd(mediaExtractor, fd, 0, length)))
			return ret;

		const size_t numTracks = AMediaExtractor_getTrackCount(mediaExtractor);
		size_t i;
		AMediaFormat* format = 0;
		const char* mime = 0;
		for (i = 0; i < numTracks; i++) {
			format = AMediaExtractor_getTrackFormat(mediaExtractor, i);
			if (!format)
				continue;
			if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime))
				continue;
			if (isAudio(mime)) {
				if ((ret = AMediaExtractor_selectTrack(mediaExtractor, i)))
					return ret;
				int sampleRate, channelCount;
				int64_t duration;
				if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate) ||
					!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channelCount) ||
					!AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration))
					continue;
				//only stereo files for now...
				if (channelCount != 2)
					return -1;
				outParams[1] = (uint64_t)sampleRate;
				outParams[2] = (uint64_t)channelCount;
				outParams[3] = (uint64_t)duration;
				break;
			}
		}
		if (i >= numTracks)
			return -1;

		mediaCodec = AMediaCodec_createDecoderByType(mime);
		if (!mediaCodec)
			return -1;

		if ((ret = AMediaCodec_configure(mediaCodec, format, 0, 0, 0)))
			return ret;

		if ((ret = AMediaCodec_start(mediaCodec)))
			return ret;

		inputOver = false;
		bufferIndex = -1;
		buffer = 0;

		if ((ret = fillInputBuffers()))
			return ret;

		return 0;
	}

	int64_t doSeek(int msec) {
		int ret;
		if ((ret = AMediaCodec_flush(mediaCodec)))
			return (int64_t)ret;
		if ((ret = AMediaExtractor_seekTo(mediaExtractor, (int64_t)msec * 1000LL, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC)))
			return (int64_t)ret;
		inputOver = false;
		bufferIndex = -1;
		buffer = 0;
		const int64_t sampleTime = AMediaExtractor_getSampleTime(mediaExtractor);
		if ((ret = fillInputBuffers()))
			return (int64_t)ret;
		return ((sampleTime < 0) ? 0x7FFFFFFFFFFFFFFFLL : sampleTime);
	}

	int nextOutputBuffer() {
		//positive: ok (odd means input over)
		//negative: error
		int ret;

		if ((ret = fillInputBuffers()))
			return ret;

		AMediaCodecBufferInfo bufferInfo;
		bufferInfo.flags = 0;
		do {
			bufferIndex = AMediaCodec_dequeueOutputBuffer(mediaCodec, &bufferInfo, OUTPUT_BUFFER_TIMEOUT_IN_US);
		} while (bufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED);

		if (bufferIndex < 0)
			return ((bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ? 0x7FFFFFFF : 0x7FFFFFFE);

		size_t outputBufferCapacity;
		buffer = AMediaCodec_getOutputBuffer(mediaCodec, bufferIndex, &outputBufferCapacity);
		if (!buffer) {
			if ((ret = AMediaCodec_releaseOutputBuffer(mediaCodec, bufferIndex, 0)))
				return ret;
			return ((bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ? 0x7FFFFFFF : 0x7FFFFFFE);
		}

		buffer += bufferInfo.offset;

		//AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4
		return ((bufferInfo.size << 1) | ((bufferInfo.flags >> 2) & 1));
	}

	void releaseOutputBuffer() {
		if (mediaCodec && bufferIndex >= 0) {
			AMediaCodec_releaseOutputBuffer(mediaCodec, bufferIndex, 0);
			bufferIndex = -1;
			buffer = 0;
		}
	}
};

int JNICALL mediaCodecPrepare(JNIEnv* env, jclass clazz, int fd, uint64_t length, jlongArray joutParams) {
	if (!fd || !joutParams || env->GetArrayLength(joutParams) < 4)
		return -1;

	uint64_t outParams[4];

	MediaCodec* nativeObj = new MediaCodec();
	const int ret = nativeObj->prepare(fd, length, outParams);

	if (ret < 0) {
		delete nativeObj;
		return ret;
	}

	outParams[0] = (uint64_t)nativeObj;
	env->SetLongArrayRegion(joutParams, 0, 4, (jlong*)outParams);

	return 0;
}

int JNICALL mediaCodecNextOutputBuffer(JNIEnv* env, jclass clazz, uint64_t nativeObj) {
	if (!nativeObj)
		return -1;

	return ((MediaCodec*)nativeObj)->nextOutputBuffer();
}

int64_t JNICALL mediaCodecSeek(JNIEnv* env, jclass clazz, uint64_t nativeObj, int msec) {
	if (!nativeObj || msec < 0)
		return -1;

	return ((MediaCodec*)nativeObj)->doSeek(msec);
}

void JNICALL mediaCodecReleaseOutputBuffer(JNIEnv* env, jclass clazz, uint64_t nativeObj) {
	if (nativeObj)
		((MediaCodec*)nativeObj)->releaseOutputBuffer();
}

void JNICALL mediaCodecRelease(JNIEnv* env, jclass clazz, uint64_t nativeObj) {
	if (nativeObj)
		delete ((MediaCodec*)nativeObj);
}