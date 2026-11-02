#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <wchar.h>
#include <atomic>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "sound.h"

extern "C" int stb_vorbis_decode_memory(const unsigned char *data, int size, int *channels, int *sample_rate, short **output);

namespace sound {
namespace {

// Footstep recordings are footstep00.ogg, footstep01.ogg, ... in this folder next to the plugin.
const wchar_t *const SOUND_FOLDER = L"takeawalk";
const unsigned MAX_FOOTSTEPS = 16;

// Recordings differ in loudness, so each is brought to this level (RMS of full scale)
// before the volume setting is applied.
const float REFERENCE_LEVEL = 0.12f;

// A landing is a footstep played this much louder.
const float LANDING_GAIN = 1.7f;

// Largest sound file read (bytes).
const long MAX_FILE_SIZE = 4 * 1024 * 1024;

// Sounds generated when no recordings are found.
const unsigned GENERATED_RATE = 22050;
const unsigned GENERATED_VARIANTS = 4;

const float TWO_PI = 6.2831853f;

#pragma pack(push, 1)

/**
 * @brief Header of a WAV file in memory. The 16-bit samples follow it.
 */
struct wave_header_t
{
	char     riff[4];
	uint32_t riff_size;
	char     wave[4];
	char     format_tag[4];
	uint32_t format_size;
	uint16_t format;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	char     data_tag[4];
	uint32_t data_size;
};

#pragma pack(pop)

// Complete WAV files, as PlaySound wants them.
uint8_t *footsteps[MAX_FOOTSTEPS];
unsigned footstep_count = 0;
uint8_t *landing_sound = NULL;
unsigned next_footstep = 0;

/**
 * @brief Wraps samples in a WAV file, scaled by a gain and limited to full scale.
 */
uint8_t *make_wave(const short *const samples, const size_t sample_count, const int channels, const int sample_rate, const float gain)
{
	const size_t data_size = sample_count * sizeof(int16_t);
	uint8_t *const file = static_cast<uint8_t *>(malloc(sizeof(wave_header_t) + data_size));
	if (! file) {
		return NULL;
	}

	wave_header_t header;
	memcpy(header.riff, "RIFF", 4);
	memcpy(header.wave, "WAVE", 4);
	memcpy(header.format_tag, "fmt ", 4);
	memcpy(header.data_tag, "data", 4);
	header.format_size = 16;
	header.format = 1;
	header.channels = static_cast<uint16_t>(channels);
	header.sample_rate = static_cast<uint32_t>(sample_rate);
	header.bits_per_sample = 16;
	header.block_align = static_cast<uint16_t>(channels * 2);
	header.byte_rate = header.sample_rate * header.block_align;
	header.data_size = static_cast<uint32_t>(data_size);
	header.riff_size = 36 + header.data_size;
	memcpy(file, &header, sizeof(header));

	int16_t *const output = reinterpret_cast<int16_t *>(file + sizeof(header));
	for (size_t i = 0; i < sample_count; ++i) {
		float value = static_cast<float>(samples[i]) * gain;
		if (value > 32767.0f) {
			value = 32767.0f;
		}
		if (value < -32768.0f) {
			value = -32768.0f;
		}
		output[i] = static_cast<int16_t>(value);
	}
	return file;
}

/**
 * @brief Gain which brings a recording to the reference level.
 */
float normalizing_gain(const short *const samples, const size_t sample_count)
{
	double sum = 0.0;
	for (size_t i = 0; i < sample_count; ++i) {
		sum += static_cast<double>(samples[i]) * samples[i];
	}
	const double level = sqrt(sum / static_cast<double>(sample_count)) / 32768.0;
	return (level > 0.0001) ? static_cast<float>(REFERENCE_LEVEL / level) : 1.0f;
}

/**
 * @brief Reads and decodes one ogg file. The samples are released with free().
 */
bool load_ogg(const wchar_t *const path, short *&samples, size_t &sample_count, int &channels, int &sample_rate)
{
	FILE *const file = _wfopen(path, L"rb");
	if (! file) {
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long size = ftell(file);
	fseek(file, 0, SEEK_SET);

	bool loaded = false;
	if ((size > 0) && (size <= MAX_FILE_SIZE)) {
		unsigned char *const data = static_cast<unsigned char *>(malloc(size));
		if (data && (fread(data, 1, size, file) == static_cast<size_t>(size))) {
			const int frames = stb_vorbis_decode_memory(data, static_cast<int>(size), &channels, &sample_rate, &samples);
			if (frames > 0) {
				sample_count = static_cast<size_t>(frames) * channels;
				loaded = true;
			}
		}
		free(data);
	}
	fclose(file);
	return loaded;
}

void load_recordings(void)
{
	for (unsigned index = 0; index < MAX_FOOTSTEPS; ++index) {
		wchar_t name[64];
		swprintf(name, 64, L"%ls\\footstep%02u.ogg", SOUND_FOLDER, index);
		wchar_t path[MAX_PATH];
		if (! config_file_path(name, path, MAX_PATH)) {
			break;
		}

		short *samples = NULL;
		size_t sample_count = 0;
		int channels = 0;
		int sample_rate = 0;
		if (! load_ogg(path, samples, sample_count, channels, sample_rate)) {
			break;
		}

		const float gain = normalizing_gain(samples, sample_count) * config.footstep_volume;
		footsteps[footstep_count] = make_wave(samples, sample_count, channels, sample_rate, gain);
		if (footsteps[footstep_count]) {
			++footstep_count;
		}
		if (! landing_sound) {
			landing_sound = make_wave(samples, sample_count, channels, sample_rate, gain * LANDING_GAIN);
		}
		free(samples);
	}
}

/**
 * @brief Makes a soft thud out of filtered noise and a low tone, for when there are no recordings.
 *
 * @param duration Length (s).
 * @param brightness How much of the noise's high end is kept, 0 to 1.
 * @param tone Frequency of the low tone (Hz).
 */
uint8_t *generate(const float duration, const float brightness, const float tone, const float gain)
{
	const size_t count = static_cast<size_t>(duration * GENERATED_RATE);
	short *const samples = static_cast<short *>(malloc(count * sizeof(short)));
	if (! samples) {
		return NULL;
	}

	uint32_t random = 0x2F6E2B1;
	float filtered = 0.0f;
	for (size_t i = 0; i < count; ++i) {
		random = random * 1664525u + 1013904223u;
		const float noise = static_cast<float>(static_cast<int32_t>(random)) / 2147483648.0f;
		filtered += (noise - filtered) * brightness;

		const float time = static_cast<float>(i) / GENERATED_RATE;
		const float attack = (time < 0.004f) ? (time / 0.004f) : 1.0f;
		const float scuff = filtered * expf(-time / (duration * 0.22f));
		const float weight = sinf(TWO_PI * tone * time) * expf(-time / (duration * 0.30f));
		samples[i] = static_cast<short>((scuff * 0.7f + weight * 0.6f) * attack * 9000.0f);
	}
	uint8_t *const wave = make_wave(samples, count, 1, GENERATED_RATE, gain);
	free(samples);
	return wave;
}

void generate_sounds(void)
{
	for (unsigned i = 0; i < GENERATED_VARIANTS; ++i) {
		const float variation = static_cast<float>(i) / GENERATED_VARIANTS;
		footsteps[footstep_count] = generate(0.11f + 0.02f * variation, 0.16f + 0.08f * variation, 95.0f - 14.0f * variation, config.footstep_volume);
		if (footsteps[footstep_count]) {
			++footstep_count;
		}
	}
	landing_sound = generate(0.19f, 0.12f, 62.0f, config.footstep_volume * LANDING_GAIN);
}


HANDLE audio_thread = NULL;
// PlaySoundW uses one process-wide playback slot. Keep footsteps from replacing
// the door/seatbelt sequence while the truck transition audio is playing.
std::atomic<bool> truck_audio_active(false);

/*
 * Play the user's original MP3 files directly through Windows MCI.
 * No decoding, resampling, gain adjustment or WAV conversion is performed:
 * the original MP3 stream is handed to the Windows audio subsystem unchanged.
 */
bool play_mp3_blocking(const wchar_t *const relative_path, const wchar_t *const alias)
{
	wchar_t path[MAX_PATH];
	if (! config_file_path(relative_path, path, MAX_PATH)) {
		log_message(SCS_LOG_TYPE_warning, "could not locate original audio file");
		return false;
	}

	wchar_t command[2 * MAX_PATH + 128];
	swprintf(command, sizeof(command) / sizeof(command[0]),
		L"open \"%ls\" type mpegvideo alias %ls", path, alias);
	MCIERROR error = mciSendStringW(command, NULL, 0, NULL);
	if (error != 0) {
		wchar_t detail[256] = {};
		mciGetErrorStringW(error, detail, 256);
		log_message(SCS_LOG_TYPE_warning, "could not open original MP3 (MCI %u)", error);
		return false;
	}

	swprintf(command, sizeof(command) / sizeof(command[0]), L"play %ls wait", alias);
	error = mciSendStringW(command, NULL, 0, NULL);
	mciSendStringW((std::wstring(L"close ") + alias).c_str(), NULL, 0, NULL);
	if (error != 0) {
		log_message(SCS_LOG_TYPE_warning, "could not play original MP3 (MCI %u)", error);
		return false;
	}
	return true;
}

DWORD WINAPI audio_thread_proc(LPVOID parameter)
{
	const INT_PTR action = reinterpret_cast<INT_PTR>(parameter);
	// 0 = startup belt, 1 = entering cab (door then belt), 2 = leaving cab (door).
	if (config.door_sounds && ((action == 1) || (action == 2))) {
		play_mp3_blocking(L"takeawalk\\enter_sequence.mp3", L"takeawalk_door");
	}
	if (config.seatbelt_sound && ((action == 0) || (action == 1))) {
		play_mp3_blocking(L"takeawalk\\seatbelt.mp3", L"takeawalk_belt");
	}
	truck_audio_active.store(false, std::memory_order_release);
	return 0;
}

void start_audio_sequence(const INT_PTR action)
{
	if (audio_thread) {
		if (WaitForSingleObject(audio_thread, 0) == WAIT_TIMEOUT) return;
		CloseHandle(audio_thread);
		audio_thread = NULL;
	}
	truck_audio_active.store(true, std::memory_order_release);
	// Stop a short footstep already using PlaySound's single playback channel.
	PlaySoundW(NULL, NULL, 0);
	audio_thread = CreateThread(NULL, 0, audio_thread_proc, reinterpret_cast<LPVOID>(action), 0, NULL);
	if (!audio_thread) {
		truck_audio_active.store(false, std::memory_order_release);
		log_message(SCS_LOG_TYPE_warning, "unable to start truck audio thread (error %lu)", GetLastError());
	}
}

void play(const uint8_t *const wave)
{
	if (wave && ! truck_audio_active.load(std::memory_order_acquire)) {
		PlaySoundW(reinterpret_cast<LPCWSTR>(wave), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
	}
}

} // namespace

void init(void)
{
	footstep_count = 0;
	landing_sound = NULL;
	next_footstep = 0;

	load_recordings();
	if (footstep_count == 0) {
		log_message(SCS_LOG_TYPE_warning, "no footstep recordings found in the plugin's \"takeawalk\" folder, using generated sounds");
		generate_sounds();
	}
}

void shutdown(void)
{
	// The sounds live in this module, so nothing may still be playing when they are released.

	PlaySoundW(NULL, NULL, 0);
	if (audio_thread) {
		WaitForSingleObject(audio_thread, INFINITE);
		CloseHandle(audio_thread);
		audio_thread = NULL;
	}
	for (unsigned i = 0; i < footstep_count; ++i) {
		free(footsteps[i]);
		footsteps[i] = NULL;
	}
	footstep_count = 0;
	free(landing_sound);
	landing_sound = NULL;
}

void footstep(void)
{
	if (footstep_count == 0) {
		return;
	}
	play(footsteps[next_footstep]);
	next_footstep = (next_footstep + 1) % footstep_count;
}

void landing(void)
{
	play(landing_sound);
}

void truck_entered(void)
{
	start_audio_sequence(1);
}

void truck_exited(void)
{
	start_audio_sequence(2);
}

void startup_buckle(void)
{
	start_audio_sequence(0);
}

} // namespace sound
